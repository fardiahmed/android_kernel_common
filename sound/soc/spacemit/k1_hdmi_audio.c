// SPDX-License-Identifier: GPL-2.0
/* SpacemiT K1 HDMI audio: ADMA + SSPA driven directly, position polled (the DMA IRQ
 * goes to the RCPU), S16 expanded to IEC subframes. Clocks on before reset release. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <soc/spacemit/spacemit_panel.h>

/* exported by spacemit_hdmi.ko: display hot-plug notifications */
extern int spacemit_hdmi_register_client(struct notifier_block *nb);
extern int spacemit_hdmi_unregister_client(struct notifier_block *nb);

/* RCPU syscon window ("clkctl") */
#define CLKCTL_HDMI_AUDIO	0x44	/* clock/reset control (reset bit 0 is owned by the reset controller) */
#define CLKCTL_FCLK		BIT(1)
#define CLKCTL_PCLK		BIT(2)
#define CLKCTL_DIV_SHIFT	4
#define CLKCTL_DIV_MASK		GENMASK(14, 4)
#define CLKCTL_MUX_MASK		GENMASK(17, 16)
#define CLKCTL_DIV_48K		511	/* 24.576MHz / 512 = 48kHz */
#define CLKCTL_ADMA_HDMI_EN	0x50

/* RCPU boot controller ("bootc"): AON peripheral clock/reset enable */
#define BOOTC_AON_CLK_RST	0x2c

/* ADMA channel registers ("adma") */
#define ADMA_BCR		0x00
#define ADMA_SAR		0x10
#define ADMA_DAR		0x20
#define ADMA_NDR		0x30
#define ADMA_DCR		0x40
#define ADMA_CUR		0x70
#define ADMA_IER		0x80
#define ADMA_ISR		0xa0
#define DCR_CH_ABORT		BIT(20)
#define DCR_FETCH_NEXT		BIT(13)
#define DCR_CH_EN		BIT(12)
#define DCR_DST_HOLD		(2 << 4)
#define DCR_SRC_INC		(0 << 2)
#define DCR_SAMPLE_32		(5 << 22)

/* SSPA transmitter ("sspa"); the FIFO data port at +0x80 is only ever a DMA target */
#define SSPA_TXD		0x80
#define SSPA_TXSP		0x8c
#define SSPA_TXFIFO_LL		0x90
#define SP_S_EN			BIT(0)
#define SP_S_RST		BIT(1)
#define SP_FFLUSH		BIT(2)
#define SP_FIX			BIT(3)
#define SP_CLKP			BIT(17)
#define SP_MSL			BIT(18)
#define SP_WEN			BIT(31)
#define TX_FIFO_THRESHOLD	0xf

/* SRAM ("sram"): descriptors first, then the raw sample ring */
#define DESC_STRIDE		0x100
#define RING_OFF		0x400
#define NPER			4
#define PER_FRAMES		480
#define RING_FRAMES		(NPER * PER_FRAMES)
#define SUBFRAME_BYTES		8	/* 2 channels x 32-bit subframe */
#define PER_RING_BYTES		(PER_FRAMES * SUBFRAME_BYTES)
#define RING_BYTES		(NPER * PER_RING_BYTES)
#define PCM_FRAME_BYTES		4	/* S16 stereo */

/* HDMI transmitter ("hdmi"): only two registers are used */
#define HDMI_PHY_STATUS		0x0c
#define HDMI_HPD		BIT(12)
#define HDMI_AUDIO_EN		0x30

struct k1ha {
	struct device *dev;
	void __iomem *adma, *sspa, *sram, *bootc, *clkctl, *hdmi;
	phys_addr_t sram_phys, sspa_phys;
	struct clk *clk_audio, *clk_pll;
	struct reset_control *rst_sys, *rst_mcu, *rst_apmu, *rst_hdmi;

	struct mutex power_lock;
	int power_refs;

	struct snd_pcm_substream *ss;
	struct hrtimer timer;
	bool running;
	unsigned int last_period;

	struct snd_soc_jack jack;
	struct notifier_block hpd_nb;
	bool connected;
};

static struct snd_soc_dai_link_component k1ha_cpu, k1ha_platform;

/* ---- power ------------------------------------------------------------- */

static int k1ha_power_on(struct k1ha *h)
{
	u32 v;
	int ret;

	ret = clk_prepare_enable(h->clk_pll);
	if (ret)
		return ret;
	ret = clk_prepare_enable(h->clk_audio);
	if (ret)
		goto err_pll;

	/* clocks are on: now it is safe to release the resets */
	ret = reset_control_deassert(h->rst_sys);
	if (ret)
		goto err_clk;
	reset_control_deassert(h->rst_mcu);
	reset_control_deassert(h->rst_apmu);

	/* HDMI audio clock: 24.576MHz source, /512 = 48kHz, then out of reset */
	v = readl(h->clkctl + CLKCTL_HDMI_AUDIO);
	v &= ~(CLKCTL_DIV_MASK | CLKCTL_MUX_MASK);
	v |= (CLKCTL_DIV_48K << CLKCTL_DIV_SHIFT) | CLKCTL_FCLK | CLKCTL_PCLK;
	writel(v, h->clkctl + CLKCTL_HDMI_AUDIO);
	usleep_range(100, 200);
	reset_control_deassert(h->rst_hdmi);
	usleep_range(100, 200);

	/* what the vendor RCPU boot does: enable the RCPU always-on peripheral clocks/resets */
	writel(0xff, h->bootc + BOOTC_AON_CLK_RST);
	return 0;

err_clk:
	clk_disable_unprepare(h->clk_audio);
err_pll:
	clk_disable_unprepare(h->clk_pll);
	return ret;
}

static void k1ha_power_off(struct k1ha *h)
{
	/* resets first: a released block whose clock is gated hangs the bus */
	reset_control_assert(h->rst_hdmi);
	reset_control_assert(h->rst_apmu);
	reset_control_assert(h->rst_sys);
	clk_disable_unprepare(h->clk_audio);
	clk_disable_unprepare(h->clk_pll);
}

/* ---- IEC subframes ----------------------------------------------------- */

static u32 k1ha_subframe(u16 sample, unsigned int iec)
{
	u32 v = (u32)sample << 8;

	v |= BIT(24);
	if (iec == 25 || iec == 32)
		v |= BIT(26);
	v |= (u32)__builtin_parity(v) << 27;
	v |= BIT(31);
	if (iec == 0)
		v |= BIT(30);
	return v;
}

/* fill the whole ring with framed silence */
static void k1ha_fill_silence(struct k1ha *h)
{
	unsigned int f;

	for (f = 0; f < RING_FRAMES; f++) {
		u32 s = k1ha_subframe(0, f % 192);

		writel(s, h->sram + RING_OFF + f * SUBFRAME_BYTES);
		writel(s, h->sram + RING_OFF + f * SUBFRAME_BYTES + 4);
	}
	wmb();
}

/* ---- DMA ---------------------------------------------------------------- */

static bool k1ha_hdmi_connected(struct k1ha *h)
{
	return readl(h->hdmi + HDMI_PHY_STATUS) & HDMI_HPD;
}

static void k1ha_dma_start(struct k1ha *h)
{
	u32 dev = h->sspa_phys + SSPA_TXD;
	u32 dcr = DCR_DST_HOLD | DCR_SRC_INC | DCR_SAMPLE_32;	/* no ABORT: it would stall the channel */
	u32 sp;
	int i;

	for (i = 0; i < NPER; i++) {
		void __iomem *d = h->sram + i * DESC_STRIDE;

		writel(PER_RING_BYTES, d + 0);
		writel(h->sram_phys + RING_OFF + i * PER_RING_BYTES, d + 4);
		writel(dev, d + 8);
		writel(h->sram_phys + ((i + 1) % NPER) * DESC_STRIDE, d + 12);
	}
	wmb();

	/* HDMI transmitter: audio on */
	writel(readl(h->hdmi + HDMI_AUDIO_EN) | 1, h->hdmi + HDMI_AUDIO_EN);

	/* SSPA transmitter: master mode, as the K3 RCPU I2S driver does */
	sp = readl(h->sspa + SSPA_TXSP);
	sp &= ~(SP_S_RST | SP_FIX | SP_CLKP);
	sp |= SP_S_EN | SP_WEN | SP_FFLUSH | SP_MSL;
	writel(TX_FIFO_THRESHOLD, h->sspa + SSPA_TXFIFO_LL);
	writel(sp, h->sspa + SSPA_TXSP);

	/* ADMA: same order as the vendor adma driver */
	writel(0, h->adma + ADMA_ISR);
	writel(h->sram_phys, h->adma + ADMA_NDR);
	writel(dcr, h->adma + ADMA_DCR);
	writel(1, h->clkctl + CLKCTL_ADMA_HDMI_EN);
	writel(dev, h->adma + ADMA_DAR);
	writel(1, h->adma + ADMA_IER);
	writel(dcr | DCR_FETCH_NEXT | DCR_CH_EN, h->adma + ADMA_DCR);
}

static void k1ha_dma_stop(struct k1ha *h)
{
	u32 sp, d;

	d = readl(h->adma + ADMA_DCR);
	writel(d | DCR_CH_ABORT, h->adma + ADMA_DCR);
	udelay(500);
	d = readl(h->adma + ADMA_DCR);
	writel(d & ~DCR_CH_EN, h->adma + ADMA_DCR);
	writel(0, h->adma + ADMA_IER);
	writel(0, h->adma + ADMA_ISR);
	if (readl(h->clkctl + CLKCTL_ADMA_HDMI_EN) & 1)
		writel(0, h->clkctl + CLKCTL_ADMA_HDMI_EN);

	sp = readl(h->sspa + SSPA_TXSP);
	sp &= ~(SP_S_EN | SP_CLKP | SP_MSL);
	sp |= SP_S_RST | SP_WEN | SP_FFLUSH;
	writel(sp, h->sspa + SSPA_TXSP);

	writel(readl(h->hdmi + HDMI_AUDIO_EN) & ~1u, h->hdmi + HDMI_AUDIO_EN);
}

static unsigned int k1ha_pos_frames(struct k1ha *h)
{
	u32 sar = readl(h->adma + ADMA_SAR);
	u32 base = h->sram_phys + RING_OFF;

	if (sar < base || sar >= base + RING_BYTES)
		return 0;
	return (sar - base) / SUBFRAME_BYTES;
}

/* No completion interrupt reaches Linux (it goes to the RCPU), so poll the DMA position. */
static enum hrtimer_restart k1ha_timer_cb(struct hrtimer *t)
{
	struct k1ha *h = container_of(t, struct k1ha, timer);
	struct snd_pcm_substream *ss = READ_ONCE(h->ss);
	unsigned int period;

	if (!ss || !READ_ONCE(h->running))
		return HRTIMER_NORESTART;

	period = k1ha_pos_frames(h) / PER_FRAMES;
	if (period != h->last_period) {
		h->last_period = period;
		snd_pcm_period_elapsed(ss);
	}
	hrtimer_forward_now(t, ms_to_ktime(2));
	return HRTIMER_RESTART;
}

/* ---- ALSA component ---------------------------------------------------- */

static const struct snd_pcm_hardware k1ha_hw = {
	.info		= SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BATCH,
	.formats	= SNDRV_PCM_FMTBIT_S16_LE,
	.rates		= SNDRV_PCM_RATE_48000,
	.rate_min	= 48000,
	.rate_max	= 48000,
	.channels_min	= 2,
	.channels_max	= 2,
	.buffer_bytes_max = RING_FRAMES * PCM_FRAME_BYTES,
	.period_bytes_min = PER_FRAMES * PCM_FRAME_BYTES,
	.period_bytes_max = PER_FRAMES * PCM_FRAME_BYTES,
	.periods_min	= NPER,
	.periods_max	= NPER,
};

/* The card owns drvdata, so our state hangs off the card. */
static struct k1ha *k1ha_from_component(struct snd_soc_component *c)
{
	return snd_soc_card_get_drvdata(c->card);
}

static int k1ha_open(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct k1ha *h = k1ha_from_component(c);
	int ret;

	/* with no sink attached the transmitter never drains and a write would block forever */
	if (!k1ha_hdmi_connected(h))
		return -ENODEV;

	ret = snd_soc_set_runtime_hwparams(ss, &k1ha_hw);
	if (ret)
		return ret;
	ret = snd_pcm_hw_constraint_integer(ss->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;

	mutex_lock(&h->power_lock);
	if (!h->power_refs) {
		ret = k1ha_power_on(h);
		if (ret) {
			mutex_unlock(&h->power_lock);
			return ret;
		}
	}
	h->power_refs++;
	mutex_unlock(&h->power_lock);

	WRITE_ONCE(h->ss, ss);
	return 0;
}

static int k1ha_close(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct k1ha *h = k1ha_from_component(c);

	WRITE_ONCE(h->running, false);
	hrtimer_cancel(&h->timer);
	WRITE_ONCE(h->ss, NULL);

	mutex_lock(&h->power_lock);
	if (h->power_refs && !--h->power_refs)
		k1ha_power_off(h);
	mutex_unlock(&h->power_lock);
	return 0;
}

static int k1ha_prepare(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct k1ha *h = k1ha_from_component(c);

	k1ha_fill_silence(h);
	h->last_period = 0;
	return 0;
}

static int k1ha_trigger(struct snd_soc_component *c, struct snd_pcm_substream *ss, int cmd)
{
	struct k1ha *h = k1ha_from_component(c);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		h->last_period = 0;
		k1ha_dma_start(h);
		WRITE_ONCE(h->running, true);
		hrtimer_start(&h->timer, ms_to_ktime(2), HRTIMER_MODE_REL_SOFT);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(h->running, false);
		hrtimer_try_to_cancel(&h->timer);
		k1ha_dma_stop(h);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t k1ha_pointer(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	struct k1ha *h = k1ha_from_component(c);

	return k1ha_pos_frames(h);
}

/* Application data goes straight into the SRAM ring, expanded to 32-bit subframes. */
static int k1ha_copy(struct snd_soc_component *c, struct snd_pcm_substream *ss, int channel,
		     unsigned long pos, struct iov_iter *iter, unsigned long bytes)
{
	struct k1ha *h = k1ha_from_component(c);
	unsigned int frame = pos / PCM_FRAME_BYTES;
	unsigned long frames = bytes / PCM_FRAME_BYTES;
	u32 out[2 * 64];
	u16 in[2 * 64];

	while (frames) {
		unsigned int n = min_t(unsigned long, frames, 64);
		unsigned int i;

		if (copy_from_iter(in, n * PCM_FRAME_BYTES, iter) != n * PCM_FRAME_BYTES)
			return -EFAULT;
		for (i = 0; i < n; i++) {
			unsigned int iec = (frame + i) % 192;

			out[2 * i] = k1ha_subframe(in[2 * i], iec);
			out[2 * i + 1] = k1ha_subframe(in[2 * i + 1], iec);
		}
		memcpy_toio(h->sram + RING_OFF + frame * SUBFRAME_BYTES, out, n * SUBFRAME_BYTES);
		frame += n;
		frames -= n;
	}
	return 0;
}

static int k1ha_pcm_construct(struct snd_soc_component *c, struct snd_soc_pcm_runtime *rtd)
{
	/* ALSA wants a runtime buffer even though .copy sends the data to SRAM */
	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV, c->dev,
				       RING_FRAMES * PCM_FRAME_BYTES, RING_FRAMES * PCM_FRAME_BYTES);
	return 0;
}

static const struct snd_soc_component_driver k1ha_component = {
	.name			= "k1-hdmi-audio",
	.open			= k1ha_open,
	.close			= k1ha_close,
	.prepare		= k1ha_prepare,
	.trigger		= k1ha_trigger,
	.pointer		= k1ha_pointer,
	.copy			= k1ha_copy,
	.pcm_new			= k1ha_pcm_construct,
	.legacy_dai_naming	= 1,
};

static struct snd_soc_dai_driver k1ha_dai = {
	.name = "k1-hdmi-audio",
	.playback = {
		.stream_name	= "HDMI Playback",
		.channels_min	= 2,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_48000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
	},
};

/* ---- card + hot-plug jack ---------------------------------------------- */

/* hdmi_jack=0 hides the HDMI jack so Android keeps the built-in speakers. */
static bool hdmi_jack = true;
static struct k1ha *k1ha_inst;	/* for the parameter callback */
static DEFINE_MUTEX(k1ha_inst_lock);

static void k1ha_report_jack(struct k1ha *h, bool connected)
{
	h->connected = connected;
	snd_soc_jack_report(&h->jack,
			    connected && READ_ONCE(hdmi_jack) ? SND_JACK_AVOUT : 0, SND_JACK_AVOUT);
}

static int k1ha_set_hdmi_jack(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (ret)
		return ret;
	mutex_lock(&k1ha_inst_lock);
	if (k1ha_inst)
		k1ha_report_jack(k1ha_inst, k1ha_inst->connected);
	mutex_unlock(&k1ha_inst_lock);
	return 0;
}

static const struct kernel_param_ops k1ha_hdmi_jack_ops = {
	.set = k1ha_set_hdmi_jack,
	.get = param_get_bool,
};
module_param_cb(hdmi_jack, &k1ha_hdmi_jack_ops, &hdmi_jack, 0644);
MODULE_PARM_DESC(hdmi_jack, "Report HDMI hot-plug as an audio jack, i.e. route audio to the monitor (default 1)");

static int k1ha_hpd_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct k1ha *h = container_of(nb, struct k1ha, hpd_nb);

	if (event == DRM_HDMI_EVENT_CONNECTED)
		k1ha_report_jack(h, true);
	else if (event == DRM_HDMI_EVENT_DISCONNECTED)
		k1ha_report_jack(h, false);
	return NOTIFY_DONE;
}

static int k1ha_card_init(struct snd_soc_pcm_runtime *rtd)
{
	struct k1ha *h = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	ret = snd_soc_card_jack_new(rtd->card, "HDMI Jack", SND_JACK_AVOUT, &h->jack);
	if (ret)
		return ret;
	k1ha_report_jack(h, k1ha_hdmi_connected(h));
	return 0;
}

static struct snd_soc_dai_link k1ha_link = {
	.name		= "HDMI",
	.stream_name	= "HDMI Playback",
	.init		= k1ha_card_init,
	.cpus		= &k1ha_cpu,
	.num_cpus	= 1,
	.codecs		= &snd_soc_dummy_dlc,
	.num_codecs	= 1,
	.platforms	= &k1ha_platform,
	.num_platforms	= 1,
};

static struct snd_soc_card k1ha_card = {
	.name		= "K1-HDMI",
	.owner		= THIS_MODULE,
	.dai_link	= &k1ha_link,
	.num_links	= 1,
};

/* ---- platform ----------------------------------------------------------- */

static void __iomem *k1ha_map(struct platform_device *pdev, const char *name, phys_addr_t *phys,
			      bool exclusive)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!res)
		return IOMEM_ERR_PTR(-ENODEV);
	if (phys)
		*phys = res->start;
	/* the syscon and display drivers own some of these windows: map without claiming */
	if (exclusive)
		return devm_ioremap_resource(&pdev->dev, res);
	return devm_ioremap(&pdev->dev, res->start, resource_size(res));
}

static void k1ha_clear_inst(void *data)
{
	mutex_lock(&k1ha_inst_lock);
	k1ha_inst = NULL;
	mutex_unlock(&k1ha_inst_lock);
}

static void k1ha_unregister_hpd(void *data)
{
	struct k1ha *h = data;

	spacemit_hdmi_unregister_client(&h->hpd_nb);
}

static int k1ha_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct k1ha *h;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	mutex_init(&h->power_lock);
	hrtimer_setup(&h->timer, k1ha_timer_cb, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);

	h->adma = k1ha_map(pdev, "adma", NULL, true);
	h->sspa = k1ha_map(pdev, "sspa", &h->sspa_phys, true);
	h->sram = k1ha_map(pdev, "sram", &h->sram_phys, true);
	h->bootc = k1ha_map(pdev, "bootc", NULL, true);
	h->clkctl = k1ha_map(pdev, "clkctl", NULL, false);
	h->hdmi = k1ha_map(pdev, "hdmi", NULL, false);
	if (IS_ERR(h->adma) || IS_ERR(h->sspa) || IS_ERR(h->sram) || IS_ERR(h->bootc) ||
	    IS_ERR(h->clkctl) || IS_ERR(h->hdmi) || !h->clkctl || !h->hdmi)
		return dev_err_probe(dev, -ENODEV, "cannot map register windows\n");

	h->clk_audio = devm_clk_get(dev, "audio");
	if (IS_ERR(h->clk_audio))
		return dev_err_probe(dev, PTR_ERR(h->clk_audio), "audio clock\n");
	h->clk_pll = devm_clk_get(dev, "pll");
	if (IS_ERR(h->clk_pll))
		return dev_err_probe(dev, PTR_ERR(h->clk_pll), "audio pll clock\n");

	h->rst_sys = devm_reset_control_get_exclusive(dev, "sys");
	h->rst_mcu = devm_reset_control_get_exclusive(dev, "mcu");
	h->rst_apmu = devm_reset_control_get_exclusive(dev, "apmu");
	h->rst_hdmi = devm_reset_control_get_exclusive(dev, "hdmi");
	if (IS_ERR(h->rst_sys) || IS_ERR(h->rst_mcu) || IS_ERR(h->rst_apmu) || IS_ERR(h->rst_hdmi))
		return dev_err_probe(dev, -ENODEV, "resets\n");

	ret = devm_snd_soc_register_component(dev, &k1ha_component, &k1ha_dai, 1);
	if (ret)
		return dev_err_probe(dev, ret, "component\n");

	h->hpd_nb.notifier_call = k1ha_hpd_event;
	ret = spacemit_hdmi_register_client(&h->hpd_nb);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, k1ha_unregister_hpd, h);
	if (ret)
		return ret;

	k1ha_cpu.of_node = dev->of_node;
	k1ha_platform.of_node = dev->of_node;
	k1ha_card.dev = dev;
	snd_soc_card_set_drvdata(&k1ha_card, h);
	ret = devm_snd_soc_register_card(dev, &k1ha_card);
	if (ret)
		return ret;

	mutex_lock(&k1ha_inst_lock);
	k1ha_inst = h;
	mutex_unlock(&k1ha_inst_lock);
	return devm_add_action_or_reset(dev, k1ha_clear_inst, h);
}

static const struct of_device_id k1ha_of_match[] = {
	{ .compatible = "spacemit,k1-hdmi-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, k1ha_of_match);

static struct platform_driver k1ha_driver = {
	.probe = k1ha_probe,
	.driver = {
		.name = "k1-hdmi-audio",
		.of_match_table = k1ha_of_match,
	},
};
module_platform_driver(k1ha_driver);

MODULE_DESCRIPTION("SpacemiT K1 HDMI audio");
MODULE_LICENSE("GPL");
