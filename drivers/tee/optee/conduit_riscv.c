// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Andes Technology Corporation
 * Copyright (C) 2025 SiFive
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mailbox/riscv-rpmi-message.h>
#include <linux/mailbox_client.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <asm/sbi.h>
#include <asm/smp.h>
#include "optee_conduit.h"

struct mpxy_tee_context {
	struct device *dev;
	struct mbox_chan **chan;
	struct mbox_client client;
	u32 max_msg_data_size;
};

static struct mpxy_tee_context *context;

/** RPMI OP-TEE ServiceGroup Service IDs */
enum mpxy_optee_service_id {
	RPMI_OPTEE_SRV_ENABLE_NOTIFICATION = 0x01,
	RPMI_OPTEE_SRV_GET_ATTRIBUTES = 0x02,
	RPMI_OPTEE_SRV_COMMUNICATE = 0x03,
	RPMI_OPTEE_SRV_MAX_COUNT,
};

struct mpxy_opteed_msg {
	unsigned long a0;
	unsigned long a1;
	unsigned long a2;
	unsigned long a3;
	unsigned long a4;
	unsigned long a5;
	unsigned long a6;
	unsigned long a7;
};

struct mpxy_opteed_resp {
	unsigned long a0;
	unsigned long a1;
	unsigned long a2;
	unsigned long a3;
};

static int hartid_to_cpuid(unsigned long hartid, unsigned int nr_cpus)
{
	for (int i = 0; i < nr_cpus; i++) {
		if (cpuid_to_hartid_map(i) == hartid)
			return i;
	}
	return -ENOENT;
}

static inline int __mpxy_mbox_send_message(struct rpmi_mbox_message *msg)
{
	int cpu, ret;

	cpu = get_cpu();
	ret = rpmi_mbox_send_message(context->chan[cpu], msg);
	put_cpu();

	return ret;
}

static void optee_riscv_sbi_mpxy(unsigned long a0, unsigned long a1,
				 unsigned long a2, unsigned long a3,
				 unsigned long a4, unsigned long a5,
				 unsigned long a6, unsigned long a7,
				 struct optee_conduit_res *res)
{
	struct mpxy_opteed_msg tx = {
		.a0 = a0, .a1 = a1, .a2 = a2, .a3 = a3,
		.a4 = a4, .a5 = a5, .a6 = a6, .a7 = a7
	};
	struct mpxy_opteed_resp *rx = (struct mpxy_opteed_resp *)res;
	struct rpmi_mbox_message msg = {0};
	int ret;

	OPTEE_CONDUIT_RES_MATCH(struct mpxy_opteed_resp);
	rpmi_mbox_init_send_with_response(&msg, RPMI_OPTEE_SRV_COMMUNICATE,
					  &tx, sizeof(tx), rx, sizeof(*rx));
	ret = __mpxy_mbox_send_message(&msg);
	if (ret) {
		pr_err_ratelimited(
			"%s: OPTEED MPXY messaging failed, errno: %d\n",
			__func__, ret);
	}
}

static int riscv_mpxy_mbox_probe(struct device *dev)
{
        struct rpmi_mbox_message msg;
        struct device_node *np, *cpu_np;
        u64 hartid, size;
        unsigned int nr_cpus;
        u32 channel_id;
        int ret, cpuid;

        /* Allocate RPXY TEE context */
        context = devm_kzalloc(dev, sizeof(*context), GFP_KERNEL);
        if (!context)
                return -ENOMEM;
        context->dev = dev;

        /* Setup mailbox client */
        context->client.dev             = context->dev;
        context->client.rx_callback     = NULL;
        context->client.tx_block        = false;
        context->client.knows_txdone    = true;
        context->client.tx_tout         = 0;

        /* Calculate how many harts we have */
        nr_cpus = 0;
        for (cpuid = 0; cpuid < NR_CPUS; cpuid++) {
                unsigned long hartid = cpuid_to_hartid_map(cpuid);

                if (hartid == INVALID_HARTID ||
                    hartid >= (unsigned long) NR_CPUS)
                        break;
                nr_cpus++;
        }
        /* Request mailbox channels per hart */
        context->chan = devm_kcalloc(dev, nr_cpus, sizeof(*context->chan),
                                     GFP_KERNEL);
        /* DT example:
         * cpu0: cpu@0 {
         *     reg = <0x0>;  // hartid = 0
         *     ...
         *     rpmi_optee_0 {
         *         compatible = "riscv,sbi-mpxy-optee";
         *         riscv,sbi-mpxy-channel-id = <0x0>;
         *         opensbi-domain-instance = <&tdomain>;
         *     };
         *     rpmi_reqfwd_0 {
         *         compatible = "riscv,sbi-mpxy-reqfwd";
         *         riscv,sbi-mpxy-channel-id = <0x10>;
         *     };
         * };
         */
        for_each_compatible_node(np, NULL, "riscv,sbi-mpxy-optee") {
                ret = of_property_read_u32(np, "riscv,sbi-mpxy-channel-id", &channel_id);
                if (ret) {
                        panic("Missing riscv,sbi-mpxy-channel-id property in node %pOF\n", np);
                }

                cpu_np = of_get_parent(np);
                if (!cpu_np) {
                        panic("Failed to get parent CPU node for %pOF\n", np);
                }

                ret = of_property_read_reg(cpu_np, 0, &hartid, &size);
                of_node_put(cpu_np);
                if (ret) {
                        panic("Failed to get hartid from parent CPU node for %pOF\n", np);
                }

                cpuid = hartid_to_cpuid(hartid, nr_cpus);
                if (cpuid < 0) {
                        panic("Invalid hartid %llu in node %pOF\n", hartid, np);
                }

                context->chan[cpuid] = mbox_request_channel(&context->client,
                                                            channel_id);
                if (IS_ERR(context->chan[cpuid])) {
                        ret = PTR_ERR(context->chan[cpuid]);
                        dev_err_probe(dev, ret, "Failed to get mbox channel\n");
                        goto fail_free_channel;
                }

                pr_info("Probed RPMI OP-TEE channel %u (dedicated to hart%llu)\n",
                        channel_id, hartid);
        }

        /* Save the maximum message data size of mailbox channel */
        rpmi_mbox_init_get_attribute(&msg, RPMI_MBOX_ATTR_MAX_MSG_DATA_SIZE);
        ret = __mpxy_mbox_send_message(&msg);
        if (ret) {
                dev_err_probe(dev, ret, "Failed to get max msg data size\n");
                goto fail_free_channel;
        }
        context->max_msg_data_size = msg.attr.value;

        return 0;

fail_free_channel:
        for (cpuid = 0; cpuid < nr_cpus; cpuid++) {
                if (context->chan[cpuid])
                        mbox_free_channel(context->chan[cpuid]);
        }

        return ret;
}

optee_invoke_fn *arch_get_invoke_func(struct device *dev)
{
	const char *method;
	int ret;

	pr_info("probing for conduit method.\n");

	if (device_property_read_string(dev, "method", &method)) {
		pr_warn("missing \"method\" property\n");
		return ERR_PTR(-ENXIO);
	}

	if (!strcmp("mpxy", method)) {
		ret = riscv_mpxy_mbox_probe(dev);
		if (ret)
			return ERR_PTR(ret);

		return optee_riscv_sbi_mpxy;
	}

	pr_warn("invalid \"method\" property: %s\n", method);
	return ERR_PTR(-EINVAL);
}
