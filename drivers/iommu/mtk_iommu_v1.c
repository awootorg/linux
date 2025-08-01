// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU API for MTK architected m4u v1 implementations
 *
 * Copyright (c) 2015-2016 MediaTek Inc.
 * Author: Honghui Zhang <honghui.zhang@mediatek.com>
 *
 * Based on driver/iommu/mtk_iommu.c
 */
#include <linux/bug.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string_choices.h>
#include <asm/barrier.h>
#include <dt-bindings/memory/mtk-memory-port.h>
#include <dt-bindings/memory/mt2701-larb-port.h>
#include <soc/mediatek/smi.h>

#if defined(CONFIG_ARM)
#include <asm/dma-iommu.h>
#else
#define arm_iommu_create_mapping(...) NULL
#define arm_iommu_attach_device(...)	-ENODEV
struct dma_iommu_mapping {
	struct iommu_domain *domain;
};
#endif

#define REG_MMU_PT_BASE_ADDR			0x000

#define F_ALL_INVLD				0x2
#define F_MMU_INV_RANGE				0x1
#define F_INVLD_EN0				BIT(0)
#define F_INVLD_EN1				BIT(1)

#define F_MMU_FAULT_VA_MSK			0xfffff000
#define MTK_PROTECT_PA_ALIGN			128

#define REG_MMU_CTRL_REG			0x210
#define F_MMU_CTRL_COHERENT_EN			BIT(8)
#define REG_MMU_IVRP_PADDR			0x214
#define REG_MMU_INT_CONTROL			0x220
#define F_INT_TRANSLATION_FAULT			BIT(0)
#define F_INT_MAIN_MULTI_HIT_FAULT		BIT(1)
#define F_INT_INVALID_PA_FAULT			BIT(2)
#define F_INT_ENTRY_REPLACEMENT_FAULT		BIT(3)
#define F_INT_TABLE_WALK_FAULT			BIT(4)
#define F_INT_TLB_MISS_FAULT			BIT(5)
#define F_INT_PFH_DMA_FIFO_OVERFLOW		BIT(6)
#define F_INT_MISS_DMA_FIFO_OVERFLOW		BIT(7)

#define F_MMU_TF_PROTECT_SEL(prot)		(((prot) & 0x3) << 5)
#define F_INT_CLR_BIT				BIT(12)

#define REG_MMU_FAULT_ST			0x224
#define REG_MMU_FAULT_VA			0x228
#define REG_MMU_INVLD_PA			0x22C
#define REG_MMU_INT_ID				0x388
#define REG_MMU_INVALIDATE			0x5c0
#define REG_MMU_INVLD_START_A			0x5c4
#define REG_MMU_INVLD_END_A			0x5c8

#define REG_MMU_INV_SEL				0x5d8
#define REG_MMU_STANDARD_AXI_MODE		0x5e8

#define REG_MMU_DCM				0x5f0
#define F_MMU_DCM_ON				BIT(1)
#define REG_MMU_CPE_DONE			0x60c
#define F_DESC_VALID				0x2
#define F_DESC_NONSEC				BIT(3)
#define MT2701_M4U_TF_LARB(TF)			(6 - (((TF) >> 13) & 0x7))
#define MT2701_M4U_TF_PORT(TF)			(((TF) >> 8) & 0xF)
/* MTK generation one iommu HW only support 4K size mapping */
#define MT2701_IOMMU_PAGE_SHIFT			12
#define MT2701_IOMMU_PAGE_SIZE			(1UL << MT2701_IOMMU_PAGE_SHIFT)
#define MT2701_LARB_NR_MAX			3

/*
 * MTK m4u support 4GB iova address space, and only support 4K page
 * mapping. So the pagetable size should be exactly as 4M.
 */
#define M2701_IOMMU_PGT_SIZE			SZ_4M

struct mtk_iommu_v1_suspend_reg {
	u32			standard_axi_mode;
	u32			dcm_dis;
	u32			ctrl_reg;
	u32			int_control0;
};

struct mtk_iommu_v1_data {
	void __iomem			*base;
	int				irq;
	struct device			*dev;
	struct clk			*bclk;
	phys_addr_t			protect_base; /* protect memory base */
	struct mtk_iommu_v1_domain	*m4u_dom;

	struct iommu_device		iommu;
	struct dma_iommu_mapping	*mapping;
	struct mtk_smi_larb_iommu	larb_imu[MTK_LARB_NR_MAX];

	struct mtk_iommu_v1_suspend_reg	reg;
};

struct mtk_iommu_v1_domain {
	spinlock_t			pgtlock; /* lock for page table */
	struct iommu_domain		domain;
	u32				*pgt_va;
	dma_addr_t			pgt_pa;
	struct mtk_iommu_v1_data	*data;
};

static int mtk_iommu_v1_bind(struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);

	return component_bind_all(dev, &data->larb_imu);
}

static void mtk_iommu_v1_unbind(struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);

	component_unbind_all(dev, &data->larb_imu);
}

static struct mtk_iommu_v1_domain *to_mtk_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct mtk_iommu_v1_domain, domain);
}

static const int mt2701_m4u_in_larb[] = {
	LARB0_PORT_OFFSET, LARB1_PORT_OFFSET,
	LARB2_PORT_OFFSET, LARB3_PORT_OFFSET
};

static inline int mt2701_m4u_to_larb(int id)
{
	int i;

	for (i = ARRAY_SIZE(mt2701_m4u_in_larb) - 1; i >= 0; i--)
		if ((id) >= mt2701_m4u_in_larb[i])
			return i;

	return 0;
}

static inline int mt2701_m4u_to_port(int id)
{
	int larb = mt2701_m4u_to_larb(id);

	return id - mt2701_m4u_in_larb[larb];
}

static void mtk_iommu_v1_tlb_flush_all(struct mtk_iommu_v1_data *data)
{
	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0,
			data->base + REG_MMU_INV_SEL);
	writel_relaxed(F_ALL_INVLD, data->base + REG_MMU_INVALIDATE);
	wmb(); /* Make sure the tlb flush all done */
}

static void mtk_iommu_v1_tlb_flush_range(struct mtk_iommu_v1_data *data,
					 unsigned long iova, size_t size)
{
	int ret;
	u32 tmp;

	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0,
		data->base + REG_MMU_INV_SEL);
	writel_relaxed(iova & F_MMU_FAULT_VA_MSK,
		data->base + REG_MMU_INVLD_START_A);
	writel_relaxed((iova + size - 1) & F_MMU_FAULT_VA_MSK,
		data->base + REG_MMU_INVLD_END_A);
	writel_relaxed(F_MMU_INV_RANGE, data->base + REG_MMU_INVALIDATE);

	ret = readl_poll_timeout_atomic(data->base + REG_MMU_CPE_DONE,
				tmp, tmp != 0, 10, 100000);
	if (ret) {
		dev_warn(data->dev,
			 "Partial TLB flush timed out, falling back to full flush\n");
		mtk_iommu_v1_tlb_flush_all(data);
	}
	/* Clear the CPE status */
	writel_relaxed(0, data->base + REG_MMU_CPE_DONE);
}

static irqreturn_t mtk_iommu_v1_isr(int irq, void *dev_id)
{
	struct mtk_iommu_v1_data *data = dev_id;
	struct mtk_iommu_v1_domain *dom = data->m4u_dom;
	u32 int_state, regval, fault_iova, fault_pa;
	unsigned int fault_larb, fault_port;

	/* Read error information from registers */
	int_state = readl_relaxed(data->base + REG_MMU_FAULT_ST);
	fault_iova = readl_relaxed(data->base + REG_MMU_FAULT_VA);

	fault_iova &= F_MMU_FAULT_VA_MSK;
	fault_pa = readl_relaxed(data->base + REG_MMU_INVLD_PA);
	regval = readl_relaxed(data->base + REG_MMU_INT_ID);
	fault_larb = MT2701_M4U_TF_LARB(regval);
	fault_port = MT2701_M4U_TF_PORT(regval);

	/*
	 * MTK v1 iommu HW could not determine whether the fault is read or
	 * write fault, report as read fault.
	 */
	if (report_iommu_fault(&dom->domain, data->dev, fault_iova,
			IOMMU_FAULT_READ))
		dev_err_ratelimited(data->dev,
			"fault type=0x%x iova=0x%x pa=0x%x larb=%d port=%d\n",
			int_state, fault_iova, fault_pa,
			fault_larb, fault_port);

	/* Interrupt clear */
	regval = readl_relaxed(data->base + REG_MMU_INT_CONTROL);
	regval |= F_INT_CLR_BIT;
	writel_relaxed(regval, data->base + REG_MMU_INT_CONTROL);

	mtk_iommu_v1_tlb_flush_all(data);

	return IRQ_HANDLED;
}

static void mtk_iommu_v1_config(struct mtk_iommu_v1_data *data,
				struct device *dev, bool enable)
{
	struct mtk_smi_larb_iommu    *larb_mmu;
	unsigned int                 larbid, portid;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	int i;

	for (i = 0; i < fwspec->num_ids; ++i) {
		larbid = mt2701_m4u_to_larb(fwspec->ids[i]);
		portid = mt2701_m4u_to_port(fwspec->ids[i]);
		larb_mmu = &data->larb_imu[larbid];

		dev_dbg(dev, "%s iommu port: %d\n",
			str_enable_disable(enable), portid);

		if (enable)
			larb_mmu->mmu |= MTK_SMI_MMU_EN(portid);
		else
			larb_mmu->mmu &= ~MTK_SMI_MMU_EN(portid);
	}
}

static int mtk_iommu_v1_domain_finalise(struct mtk_iommu_v1_data *data)
{
	struct mtk_iommu_v1_domain *dom = data->m4u_dom;

	spin_lock_init(&dom->pgtlock);

	dom->pgt_va = dma_alloc_coherent(data->dev, M2701_IOMMU_PGT_SIZE,
					 &dom->pgt_pa, GFP_KERNEL);
	if (!dom->pgt_va)
		return -ENOMEM;

	writel(dom->pgt_pa, data->base + REG_MMU_PT_BASE_ADDR);

	dom->data = data;

	return 0;
}

static struct iommu_domain *mtk_iommu_v1_domain_alloc_paging(struct device *dev)
{
	struct mtk_iommu_v1_domain *dom;

	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return NULL;

	dom->domain.pgsize_bitmap = MT2701_IOMMU_PAGE_SIZE;

	return &dom->domain;
}

static void mtk_iommu_v1_domain_free(struct iommu_domain *domain)
{
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	struct mtk_iommu_v1_data *data = dom->data;

	dma_free_coherent(data->dev, M2701_IOMMU_PGT_SIZE,
			dom->pgt_va, dom->pgt_pa);
	kfree(to_mtk_domain(domain));
}

static int mtk_iommu_v1_attach_device(struct iommu_domain *domain, struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_iommu_priv_get(dev);
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	struct dma_iommu_mapping *mtk_mapping;
	int ret;

	/* Only allow the domain created internally. */
	mtk_mapping = data->mapping;
	if (mtk_mapping->domain != domain)
		return 0;

	if (!data->m4u_dom) {
		data->m4u_dom = dom;
		ret = mtk_iommu_v1_domain_finalise(data);
		if (ret) {
			data->m4u_dom = NULL;
			return ret;
		}
	}

	mtk_iommu_v1_config(data, dev, true);
	return 0;
}

static int mtk_iommu_v1_identity_attach(struct iommu_domain *identity_domain,
					struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_iommu_priv_get(dev);

	mtk_iommu_v1_config(data, dev, false);
	return 0;
}

static struct iommu_domain_ops mtk_iommu_v1_identity_ops = {
	.attach_dev = mtk_iommu_v1_identity_attach,
};

static struct iommu_domain mtk_iommu_v1_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &mtk_iommu_v1_identity_ops,
};

static int mtk_iommu_v1_map(struct iommu_domain *domain, unsigned long iova,
			    phys_addr_t paddr, size_t pgsize, size_t pgcount,
			    int prot, gfp_t gfp, size_t *mapped)
{
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	unsigned long flags;
	unsigned int i;
	u32 *pgt_base_iova = dom->pgt_va + (iova  >> MT2701_IOMMU_PAGE_SHIFT);
	u32 pabase = (u32)paddr;

	spin_lock_irqsave(&dom->pgtlock, flags);
	for (i = 0; i < pgcount; i++) {
		if (pgt_base_iova[i])
			break;
		pgt_base_iova[i] = pabase | F_DESC_VALID | F_DESC_NONSEC;
		pabase += MT2701_IOMMU_PAGE_SIZE;
	}

	spin_unlock_irqrestore(&dom->pgtlock, flags);

	*mapped = i * MT2701_IOMMU_PAGE_SIZE;
	mtk_iommu_v1_tlb_flush_range(dom->data, iova, *mapped);

	return i == pgcount ? 0 : -EEXIST;
}

static size_t mtk_iommu_v1_unmap(struct iommu_domain *domain, unsigned long iova,
				 size_t pgsize, size_t pgcount,
				 struct iommu_iotlb_gather *gather)
{
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	unsigned long flags;
	u32 *pgt_base_iova = dom->pgt_va + (iova  >> MT2701_IOMMU_PAGE_SHIFT);
	size_t size = pgcount * MT2701_IOMMU_PAGE_SIZE;

	spin_lock_irqsave(&dom->pgtlock, flags);
	memset(pgt_base_iova, 0, pgcount * sizeof(u32));
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	mtk_iommu_v1_tlb_flush_range(dom->data, iova, size);

	return size;
}

static phys_addr_t mtk_iommu_v1_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	unsigned long flags;
	phys_addr_t pa;

	spin_lock_irqsave(&dom->pgtlock, flags);
	pa = *(dom->pgt_va + (iova >> MT2701_IOMMU_PAGE_SHIFT));
	pa = pa & (~(MT2701_IOMMU_PAGE_SIZE - 1));
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	return pa;
}

static const struct iommu_ops mtk_iommu_v1_ops;

/*
 * MTK generation one iommu HW only support one iommu domain, and all the client
 * sharing the same iova address space.
 */
static int mtk_iommu_v1_create_mapping(struct device *dev,
				       const struct of_phandle_args *args)
{
	struct mtk_iommu_v1_data *data;
	struct platform_device *m4updev;
	struct dma_iommu_mapping *mtk_mapping;
	int ret;

	if (args->args_count != 1) {
		dev_err(dev, "invalid #iommu-cells(%d) property for IOMMU\n",
			args->args_count);
		return -EINVAL;
	}

	ret = iommu_fwspec_init(dev, of_fwnode_handle(args->np));
	if (ret)
		return ret;

	if (!dev_iommu_priv_get(dev)) {
		/* Get the m4u device */
		m4updev = of_find_device_by_node(args->np);
		if (WARN_ON(!m4updev))
			return -EINVAL;

		dev_iommu_priv_set(dev, platform_get_drvdata(m4updev));
	}

	ret = iommu_fwspec_add_ids(dev, args->args, 1);
	if (ret)
		return ret;

	data = dev_iommu_priv_get(dev);
	mtk_mapping = data->mapping;
	if (!mtk_mapping) {
		/* MTK iommu support 4GB iova address space. */
		mtk_mapping = arm_iommu_create_mapping(dev, 0, 1ULL << 32);
		if (IS_ERR(mtk_mapping))
			return PTR_ERR(mtk_mapping);

		data->mapping = mtk_mapping;
	}

	return 0;
}

static struct iommu_device *mtk_iommu_v1_probe_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = NULL;
	struct of_phandle_args iommu_spec;
	struct mtk_iommu_v1_data *data;
	int err, idx = 0, larbid, larbidx;
	struct device_link *link;
	struct device *larbdev;

	while (!of_parse_phandle_with_args(dev->of_node, "iommus",
					   "#iommu-cells",
					   idx, &iommu_spec)) {

		err = mtk_iommu_v1_create_mapping(dev, &iommu_spec);
		of_node_put(iommu_spec.np);
		if (err)
			return ERR_PTR(err);

		/* dev->iommu_fwspec might have changed */
		fwspec = dev_iommu_fwspec_get(dev);
		idx++;
	}

	if (!fwspec)
		return ERR_PTR(-ENODEV);

	data = dev_iommu_priv_get(dev);

	/* Link the consumer device with the smi-larb device(supplier) */
	larbid = mt2701_m4u_to_larb(fwspec->ids[0]);
	if (larbid >= MT2701_LARB_NR_MAX)
		return ERR_PTR(-EINVAL);

	for (idx = 1; idx < fwspec->num_ids; idx++) {
		larbidx = mt2701_m4u_to_larb(fwspec->ids[idx]);
		if (larbid != larbidx) {
			dev_err(dev, "Can only use one larb. Fail@larb%d-%d.\n",
				larbid, larbidx);
			return ERR_PTR(-EINVAL);
		}
	}

	larbdev = data->larb_imu[larbid].dev;
	if (!larbdev)
		return ERR_PTR(-EINVAL);

	link = device_link_add(dev, larbdev,
			       DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS);
	if (!link)
		dev_err(dev, "Unable to link %s\n", dev_name(larbdev));

	return &data->iommu;
}

static void mtk_iommu_v1_probe_finalize(struct device *dev)
{
	__maybe_unused struct mtk_iommu_v1_data *data = dev_iommu_priv_get(dev);
	int err;

	err = arm_iommu_attach_device(dev, data->mapping);
	if (err)
		dev_err(dev, "Can't create IOMMU mapping - DMA-OPS will not work\n");
}

static void mtk_iommu_v1_release_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct mtk_iommu_v1_data *data;
	struct device *larbdev;
	unsigned int larbid;

	data = dev_iommu_priv_get(dev);
	larbid = mt2701_m4u_to_larb(fwspec->ids[0]);
	larbdev = data->larb_imu[larbid].dev;
	device_link_remove(dev, larbdev);
}

static int mtk_iommu_v1_hw_init(const struct mtk_iommu_v1_data *data)
{
	u32 regval;
	int ret;

	ret = clk_prepare_enable(data->bclk);
	if (ret) {
		dev_err(data->dev, "Failed to enable iommu bclk(%d)\n", ret);
		return ret;
	}

	regval = F_MMU_CTRL_COHERENT_EN | F_MMU_TF_PROTECT_SEL(2);
	writel_relaxed(regval, data->base + REG_MMU_CTRL_REG);

	regval = F_INT_TRANSLATION_FAULT |
		F_INT_MAIN_MULTI_HIT_FAULT |
		F_INT_INVALID_PA_FAULT |
		F_INT_ENTRY_REPLACEMENT_FAULT |
		F_INT_TABLE_WALK_FAULT |
		F_INT_TLB_MISS_FAULT |
		F_INT_PFH_DMA_FIFO_OVERFLOW |
		F_INT_MISS_DMA_FIFO_OVERFLOW;
	writel_relaxed(regval, data->base + REG_MMU_INT_CONTROL);

	/* protect memory,hw will write here while translation fault */
	writel_relaxed(data->protect_base,
			data->base + REG_MMU_IVRP_PADDR);

	writel_relaxed(F_MMU_DCM_ON, data->base + REG_MMU_DCM);

	if (devm_request_irq(data->dev, data->irq, mtk_iommu_v1_isr, 0,
			     dev_name(data->dev), (void *)data)) {
		writel_relaxed(0, data->base + REG_MMU_PT_BASE_ADDR);
		clk_disable_unprepare(data->bclk);
		dev_err(data->dev, "Failed @ IRQ-%d Request\n", data->irq);
		return -ENODEV;
	}

	return 0;
}

static const struct iommu_ops mtk_iommu_v1_ops = {
	.identity_domain = &mtk_iommu_v1_identity_domain,
	.domain_alloc_paging = mtk_iommu_v1_domain_alloc_paging,
	.probe_device	= mtk_iommu_v1_probe_device,
	.probe_finalize = mtk_iommu_v1_probe_finalize,
	.release_device	= mtk_iommu_v1_release_device,
	.device_group	= generic_device_group,
	.owner          = THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= mtk_iommu_v1_attach_device,
		.map_pages	= mtk_iommu_v1_map,
		.unmap_pages	= mtk_iommu_v1_unmap,
		.iova_to_phys	= mtk_iommu_v1_iova_to_phys,
		.free		= mtk_iommu_v1_domain_free,
	}
};

static const struct of_device_id mtk_iommu_v1_of_ids[] = {
	{ .compatible = "mediatek,mt2701-m4u", },
	{}
};
MODULE_DEVICE_TABLE(of, mtk_iommu_v1_of_ids);

static const struct component_master_ops mtk_iommu_v1_com_ops = {
	.bind		= mtk_iommu_v1_bind,
	.unbind		= mtk_iommu_v1_unbind,
};

static int mtk_iommu_v1_probe(struct platform_device *pdev)
{
	struct device			*dev = &pdev->dev;
	struct mtk_iommu_v1_data	*data;
	struct resource			*res;
	struct component_match		*match = NULL;
	void				*protect;
	int				larb_nr, ret, i;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;

	/* Protect memory. HW will access here while translation fault.*/
	protect = devm_kcalloc(dev, 2, MTK_PROTECT_PA_ALIGN,
			       GFP_KERNEL | GFP_DMA);
	if (!protect)
		return -ENOMEM;
	data->protect_base = ALIGN(virt_to_phys(protect), MTK_PROTECT_PA_ALIGN);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	data->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->base))
		return PTR_ERR(data->base);

	data->irq = platform_get_irq(pdev, 0);
	if (data->irq < 0)
		return data->irq;

	data->bclk = devm_clk_get(dev, "bclk");
	if (IS_ERR(data->bclk))
		return PTR_ERR(data->bclk);

	larb_nr = of_count_phandle_with_args(dev->of_node,
					     "mediatek,larbs", NULL);
	if (larb_nr < 0)
		return larb_nr;

	for (i = 0; i < larb_nr; i++) {
		struct device_node *larbnode;
		struct platform_device *plarbdev;

		larbnode = of_parse_phandle(dev->of_node, "mediatek,larbs", i);
		if (!larbnode)
			return -EINVAL;

		if (!of_device_is_available(larbnode)) {
			of_node_put(larbnode);
			continue;
		}

		plarbdev = of_find_device_by_node(larbnode);
		if (!plarbdev) {
			of_node_put(larbnode);
			return -ENODEV;
		}
		if (!plarbdev->dev.driver) {
			of_node_put(larbnode);
			return -EPROBE_DEFER;
		}
		data->larb_imu[i].dev = &plarbdev->dev;

		component_match_add_release(dev, &match, component_release_of,
					    component_compare_of, larbnode);
	}

	platform_set_drvdata(pdev, data);

	ret = mtk_iommu_v1_hw_init(data);
	if (ret)
		return ret;

	ret = iommu_device_sysfs_add(&data->iommu, &pdev->dev, NULL,
				     dev_name(&pdev->dev));
	if (ret)
		goto out_clk_unprepare;

	ret = iommu_device_register(&data->iommu, &mtk_iommu_v1_ops, dev);
	if (ret)
		goto out_sysfs_remove;

	ret = component_master_add_with_match(dev, &mtk_iommu_v1_com_ops, match);
	if (ret)
		goto out_dev_unreg;
	return ret;

out_dev_unreg:
	iommu_device_unregister(&data->iommu);
out_sysfs_remove:
	iommu_device_sysfs_remove(&data->iommu);
out_clk_unprepare:
	clk_disable_unprepare(data->bclk);
	return ret;
}

static void mtk_iommu_v1_remove(struct platform_device *pdev)
{
	struct mtk_iommu_v1_data *data = platform_get_drvdata(pdev);

	iommu_device_sysfs_remove(&data->iommu);
	iommu_device_unregister(&data->iommu);

	clk_disable_unprepare(data->bclk);
	devm_free_irq(&pdev->dev, data->irq, data);
	component_master_del(&pdev->dev, &mtk_iommu_v1_com_ops);
}

static int __maybe_unused mtk_iommu_v1_suspend(struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);
	struct mtk_iommu_v1_suspend_reg *reg = &data->reg;
	void __iomem *base = data->base;

	reg->standard_axi_mode = readl_relaxed(base +
					       REG_MMU_STANDARD_AXI_MODE);
	reg->dcm_dis = readl_relaxed(base + REG_MMU_DCM);
	reg->ctrl_reg = readl_relaxed(base + REG_MMU_CTRL_REG);
	reg->int_control0 = readl_relaxed(base + REG_MMU_INT_CONTROL);
	return 0;
}

static int __maybe_unused mtk_iommu_v1_resume(struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);
	struct mtk_iommu_v1_suspend_reg *reg = &data->reg;
	void __iomem *base = data->base;

	writel_relaxed(data->m4u_dom->pgt_pa, base + REG_MMU_PT_BASE_ADDR);
	writel_relaxed(reg->standard_axi_mode,
		       base + REG_MMU_STANDARD_AXI_MODE);
	writel_relaxed(reg->dcm_dis, base + REG_MMU_DCM);
	writel_relaxed(reg->ctrl_reg, base + REG_MMU_CTRL_REG);
	writel_relaxed(reg->int_control0, base + REG_MMU_INT_CONTROL);
	writel_relaxed(data->protect_base, base + REG_MMU_IVRP_PADDR);
	return 0;
}

static const struct dev_pm_ops mtk_iommu_v1_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_iommu_v1_suspend, mtk_iommu_v1_resume)
};

static struct platform_driver mtk_iommu_v1_driver = {
	.probe	= mtk_iommu_v1_probe,
	.remove = mtk_iommu_v1_remove,
	.driver	= {
		.name = "mtk-iommu-v1",
		.of_match_table = mtk_iommu_v1_of_ids,
		.pm = &mtk_iommu_v1_pm_ops,
	}
};
module_platform_driver(mtk_iommu_v1_driver);

MODULE_DESCRIPTION("IOMMU API for MediaTek M4U v1 implementations");
MODULE_LICENSE("GPL v2");
