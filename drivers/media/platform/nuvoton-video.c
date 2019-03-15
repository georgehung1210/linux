// SPDX-License-Identifier: GPL-2.0+

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#define DEVICE_NAME			"nuvoton-video"
#define NUM_FORMATS 1

#define MAX_FRAME_RATE			60
#define MAX_WIDTH	1920
#define MAX_HEIGHT	1200
#define MIN_WIDTH	640
#define MIN_HEIGHT	480
#define MIN_LP	512
#define MAX_LP	4096

#define RECT_W	16
#define RECT_H	16

#define VIDEO_BUSY_TIMEOUT	msecs_to_jiffies(100)
#define STOP_TIMEOUT			msecs_to_jiffies(500)
#define RESOLUTION_CHANGE_DELAY		msecs_to_jiffies(500)

#define VCD_MAX_SRC_BUFFER_SIZE	0x500000 /* 1920 * 1200, depth 16 */

/* VCD  Register */
#define VCD_DIFF_TBL 0x0000
#define VCD_FBA_ADR	0x8000
#define VCD_FBB_ADR	0x8004

#define VCD_FB_LP	0x8008
#define  VCD_FB_LP_MASK 0xffff
#define  VCD_FBB_LP_OFFSET 16

#define VCD_CAP_RES	0x800C
#define  VCD_CAPRES_MASK 0x7ff

#define VCD_DVO_DEL 0x8010
#define  VCD_DVO_DEL_VERT_HOFF GENMASK(31, 27)
#define  VCD_DVO_DEL_MASK 0x7ff
#define  VCD_DVO_DEL_VERT_HOFF_OFFSET 27
#define  VCD_DVO_DEL_VSYNC_DEL_OFFSET 16
#define  VCD_DVO_DEL_HSYNC_DEL_OFFSET 0

#define VCD_MODE	0x8014
#define  VCD_MODE_VCDE	BIT(0)
#define  VCD_MODE_CM565	BIT(1)
#define  VCD_MODE_IDBC	BIT(3)
#define  VCD_MODE_COLOR_CNVRT	GENMASK(5, 4)
#define  VCD_MODE_DAT_INV	BIT(6)
#define  VCD_MODE_CLK_EDGE	BIT(8)
#define  VCD_MODE_HS_EDGE	BIT(9)
#define  VCD_MODE_VS_EDGE	BIT(10)
#define  VCD_MODE_DE_HS		BIT(11)
#define  VCD_MODE_KVM_BW_SET	BIT(16)
#define  VCD_MODE_COLOR_NORM	0x0
#define  VCD_MODE_COLOR_222		0x1
#define  VCD_MODE_COLOR_666		0x2
#define  VCD_MODE_COLOR_888		0x3
#define  VCD_MODE_CM_555		0x0
#define  VCD_MODE_CM_565		0x1
#define  VCD_MODE_COLOR_CNVRT_OFFSET 4

#define VCD_CMD			0x8018
#define  VCD_CMD_GO	BIT(0)
#define  VCD_CMD_RST	BIT(1)
#define  VCD_CMD_OP_MASK	0x70
#define  VCD_CMD_OP_OFFSET	4
#define  VCD_CMD_OP_CAPTURE	0
#define  VCD_CMD_OP_COMPARE_TWO	1
#define  VCD_CMD_OP_COMPARE	2

#define	VCD_STAT		0x801C
#define	 VCD_STAT_IRQ	BIT(31)
#define	 VCD_STAT_BUSY	BIT(30)
#define	 VCD_STAT_BSD3	BIT(13)
#define	 VCD_STAT_BSD2	BIT(12)
#define	 VCD_STAT_HSYNC	BIT(11)
#define	 VCD_STAT_VSYNC	BIT(10)
#define	 VCD_STAT_HLC_CHG	BIT(9)
#define	 VCD_STAT_HAC_CHG	BIT(8)
#define	 VCD_STAT_HHT_CHG	BIT(7)
#define	 VCD_STAT_HCT_CHG	BIT(6)
#define	 VCD_STAT_VHT_CHG	BIT(5)
#define	 VCD_STAT_VCT_CHG	BIT(4)
#define	 VCD_STAT_IFOR	BIT(3)
#define	 VCD_STAT_IFOT	BIT(2)
#define	 VCD_STAT_BSD1	BIT(1)
#define	 VCD_STAT_DONE	BIT(0)
#define	 VCD_STAT_CLEAR	0x3FFF
#define	 VCD_STAT_CURR_LINE_OFFSET 16
#define	 VCD_STAT_CURR_LINE 0x7ff0000

#define VCD_INTE	0x8020
#define  VCD_INTE_DONE_IE	BIT(0)
#define  VCD_INTE_BSD_IE	BIT(1)
#define  VCD_INTE_IFOT_IE	BIT(2)
#define  VCD_INTE_IFOR_IE	BIT(3)
#define  VCD_INTE_VCT_CHG_IE	BIT(4)
#define  VCD_INTE_VHT_CHG_IE	BIT(5)
#define  VCD_INTE_HCT_CHG_IE	BIT(6)
#define  VCD_INTE_HHT_CHG_IE	BIT(7)
#define  VCD_INTE_HAC_CHG_IE	BIT(8)
#define  VCD_INTE_HLC_CHG_IE	BIT(9)
#define  VCD_INTE_VSYNC_IE	BIT(10)
#define  VCD_INTE_HSYNC_IE	BIT(11)
#define  VCD_INTE_BSD2_IE	BIT(12)
#define  VCD_INTE_BSD3_IE	BIT(13)
#define  VCD_INTE_VAL	(VCD_INTE_DONE_IE)

#define VCD_RCHG	0x8028
#define VCD_RCHG_TIM_PRSCL_OFFSET 9
#define VCD_RCHG_IG_CHG2_OFFSET 6
#define VCD_RCHG_IG_CHG1_OFFSET 3
#define VCD_RCHG_IG_CHG0_OFFSET 0
#define VCD_RCHG_TIM_PRSCL  GENMASK(12, VCD_RCHG_TIM_PRSCL_OFFSET)
#define VCD_RCHG_IG_CHG2  GENMASK(8, VCD_RCHG_IG_CHG2_OFFSET)
#define VCD_RCHG_IG_CHG1  GENMASK(5, VCD_RCHG_IG_CHG1_OFFSET)
#define VCD_RCHG_IG_CHG0  GENMASK(2, VCD_RCHG_IG_CHG0_OFFSET)

#define VCD_HOR_CYC_TIM	0x802C
#define VCD_HOR_CYC_TIM_NEW	BIT(31)
#define VCD_HOR_CYC_TIM_HCT_DIF	BIT(30)
#define VCD_HOR_CYC_TIM_VALUE	GENMASK(11, 0)

#define VCD_HOR_CYC_LAST	0x8030
#define VCD_HOR_CYC_LAST_VALUE	GENMASK(11, 0)

#define VCD_HOR_HI_TIM	0x8034
#define VCD_HOR_HI_TIM_NEW	BIT(31)
#define VCD_HOR_HI_TIM_HHT_DIF	BIT(30)
#define VCD_HOR_HI_TIM_VALUE	GENMASK(11, 0)

#define VCD_HOR_HI_LAST	0x8038
#define VCD_HOR_HI_LAST_VALUE	GENMASK(11, 0)

#define VCD_VER_CYC_TIM	0x803C
#define VCD_VER_CYC_TIM_NEW	BIT(31)
#define VCD_VER_CYC_TIM_VCT_DIF	BIT(30)
#define VCD_VER_CYC_TIM_VALUE	GENMASK(23, 0)

#define VCD_VER_CYC_LAST	0x8040
#define VCD_VER_CYC_LAST_VALUE	GENMASK(23, 0)

#define VCD_VER_HI_TIM	0x8044
#define VCD_VER_HI_TIM_NEW	BIT(31)
#define VCD_VER_HI_TIM_VHT_DIF	BIT(30)
#define VCD_VER_HI_TIM_VALUE	GENMASK(23, 0)

#define VCD_VER_HI_LAST	0x8048
#define VCD_VER_HI_LAST_VALUE	GENMASK(23, 0)

#define VCD_HOR_AC_TIM	0x804C
#define VCD_HOR_AC_TIM_NEW	BIT(31)
#define VCD_HOR_AC_TIM_HAC_DIF	BIT(30)
#define VCD_HOR_AC_TIM_VALUE	GENMASK(13, 0)

#define VCD_HOR_AC_LAST	0x8050
#define VCD_HOR_AC_LAST_VALUE	GENMASK(13, 0)

#define VCD_HOR_LIN_TIM	0x8054
#define VCD_HOR_LIN_TIM_NEW	BIT(31)
#define VCD_HOR_LIN_TIM_HLC_DIF	BIT(30)
#define VCD_HOR_LIN_TIM_VALUE	GENMASK(11, 0)

#define VCD_HOR_LIN_LAST	0x8058
#define VCD_HOR_LIN_LAST_VALUE	GENMASK(11, 0)

#define VCD_FIFO		0x805C
#define  VCD_FIFO_TH	0x100350ff

#define VCD_KVM_BW_PCLK 120000000UL

/* GCR  Register */
#define INTCR 0x3c
#define  INTCR_GFXIFDIS	(BIT(8) | BIT(9))
#define  INTCR_LDDRB	BIT(18)
#define  INTCR_DACOFF	BIT(15)
#define  INTCR_DEHS	BIT(27)

#define INTCR2 0x60
#define  INTCR2_GIRST2	BIT(2)
#define  INTCR2_GIHCRST	BIT(5)
#define  INTCR2_GIVCRST	BIT(6)

#define MFSEL1 0x0c
#define  MFSEL1_DVH1SEL	BIT(27)

/* GFXI Register */
#define GFXI_START 0xE000
#define GFXI_FIFO 0xE050
#define GFXI_MASK 0x00FF

#define DISPST	0
#define  DISPST_MGAMODE BIT(7)

#define HVCNTL	0x10
#define  HVCNTL_MASK	0xff
#define HVCNTH	0x14
#define  HVCNTH_MASK	0x07
#define HBPCNTL	0x18
#define  HBPCNTL_MASK	0xff
#define HBPCNTH	0x1c
#define  HBPCNTH_MASK	0x01
#define VVCNTL	0x20
#define  VVCNTL_MASK	0xff
#define VVCNTH	0x24
#define  VVCNTH_MASK	0x07
#define VPBCNTL	0x28
#define  VPBCNTL_MASK	0xff
#define VPBCNTH	0x2C
#define  VPBCNTH_MASK	0x01

#define GPLLINDIV	0x40
#define  GPLLINDIV_MASK	0x3f
#define  GPLLFBDV8_MASK	0x80
#define  GPLLINDIV_OFFSET	0
#define  GPLLFBDV8_OFFSET	7

#define GPLLFBDIV	0x44
#define  GPLLFBDIV_MASK	0xff

#define GPLLST	0x48
#define  GPLLFBDV109_MASK	0xc0
#define  GPLLFBDV109_OFFSET	6
#define  GPLLST_PLLOTDIV1_MASK	0x07
#define  GPLLST_PLLOTDIV2_MASK	0x38
#define  GPLLST_PLLOTDIV1_OFFSET	0
#define  GPLLST_PLLOTDIV2_OFFSET	3

#define SWAP16(x)	cpu_to_be16((u16)(x))
#define SWAP32(x)	cpu_to_be32((u32)(x))

#define to_nuvoton_video(x) container_of((x), struct nuvoton_video, v4l2_dev)

struct nuvoton_format {
	char *name;
	__u32 fourcc;
	int colorspace;
	int depth;
	u32 type;
	u32 flags;
};

struct nuvoton_video_addr {
	unsigned int size;
	dma_addr_t dma;
	void *virt;
};

struct nuvoton_video_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head link;
};

enum {
	VIDEO_RES_CHANGE,
	VIDEO_STREAMING,
	VIDEO_FRAME_INPRG,
	VIDEO_STOPPED,
};

struct rect_list {
	struct v4l2_clip clip;
	struct list_head list;
};

struct rect_list_info {
	struct rect_list *list;
	struct rect_list *first;
	struct list_head *head;
	int index;
	int tile_perline;
	int tile_perrow;
	int offset_perline;
	int tile_size;
	int tile_cnt;
};

#define to_nuvoton_video_buffer(x) \
	container_of((x), struct nuvoton_video_buffer, vb)

struct nuvoton_video {
	void __iomem *base;
	struct regmap *gcr_regmap;
	struct regmap *gfx_regmap;

	struct device *dev;
	struct v4l2_ctrl_handler v4l2_ctrl;
	struct v4l2_device v4l2_dev;
	struct v4l2_pix_format pix_fmt;
	struct v4l2_bt_timings active_timings;
	struct v4l2_bt_timings detected_timings;
	u32 v4l2_input_status;
	struct vb2_queue queue;
	struct video_device vdev;
	struct mutex video_lock;	/* v4l2 and videobuf2 lock */

	wait_queue_head_t wait;
	struct list_head buffers;
	spinlock_t lock;	/* buffer list lock */
	struct delayed_work res_work;
	unsigned long flags;
	unsigned int sequence;

	unsigned int max_buffer_size;
	struct nuvoton_video_addr src;

	int frame_rate;
	int buffer_index;
	u32 cmd;
	u32 bytesperline;
	u8 bytesperpixel;
	u32 buffer_offset;
	u32 hac_last;
	u32 rect_cnt;
	u8 num_buffers;
	struct list_head *list;
	u32 *clipcount;
	int refresh;
};

static const struct v4l2_dv_timings_cap nuvoton_video_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	.bt = {
		.min_width = MIN_WIDTH,
		.max_width = MAX_WIDTH,
		.min_height = MIN_HEIGHT,
		.max_height = MAX_HEIGHT,
		.min_pixelclock = 6574080, /* 640 x 480 x 24Hz */
		.max_pixelclock = 138240000, /* 1920 x 1200 x 60Hz */
		.standards = V4L2_DV_BT_STD_CEA861 | V4L2_DV_BT_STD_DMT |
			V4L2_DV_BT_STD_CVT | V4L2_DV_BT_STD_GTF,
		.capabilities = V4L2_DV_BT_CAP_PROGRESSIVE |
			V4L2_DV_BT_CAP_REDUCED_BLANKING |
			V4L2_DV_BT_CAP_CUSTOM,
	},
};

static void nuvoton_video_update(struct nuvoton_video *video,
				 u32 reg, u32 clear, u32 bits)
{
	u32 t = readl(video->base + reg);
	u32 before = t;

	t &= ~clear;
	t |= bits;
	writel(t, video->base + reg);
	dev_dbg(video->dev, "update %03x[%08x -> %08x]\n", reg, before,
		readl(video->base + reg));
}

static u32 nuvoton_video_read(struct nuvoton_video *video, u32 reg)
{
	u32 t = readl(video->base + reg);

	dev_dbg(video->dev, "read %03x[%08x]\n", reg, t);
	return t;
}

static void nuvoton_video_write(struct nuvoton_video *video, u32 reg, u32 val)
{
	writel(val, video->base + reg);
	dev_dbg(video->dev, "write %03x[%08x]\n", reg,
		readl(video->base + reg));
}

static bool nuvoton_video_alloc_buf(struct nuvoton_video *video,
				    struct nuvoton_video_addr *addr,
				  unsigned int size)
{
	if (size > VCD_MAX_SRC_BUFFER_SIZE)
		size = VCD_MAX_SRC_BUFFER_SIZE;

	addr->virt = dma_alloc_coherent(video->dev, size, &addr->dma,
					GFP_KERNEL);
	if (!addr->virt)
		return false;

	addr->size = size;
	return true;
}

static void nuvoton_video_free_buf(struct nuvoton_video *video,
				   struct nuvoton_video_addr *addr)
{
	dma_free_coherent(video->dev, addr->size, addr->virt, addr->dma);
	addr->size = 0;
	addr->dma = 0ULL;
	addr->virt = NULL;
}

static void nuvoton_video_free_diff_table(struct nuvoton_video *video)
{
	struct list_head *head, *pos, *nx;
	struct rect_list *tmp;
	int i;

	for (i = 0 ; i < video->num_buffers ; i++) {
		head = &video->list[i];
		list_for_each_safe(pos, nx, head) {
			tmp = list_entry(pos, struct rect_list, list);
			if (tmp) {
				list_del(&tmp->list);
				kfree(tmp);
			}
		}
	}
}

static int
nuvoton_video_add_rect(struct nuvoton_video *video,
		       int index, u32 x, u32 y, u32 w, u32 h)
{
	struct list_head *head = &video->list[index];
	struct rect_list *list = NULL;
	struct v4l2_rect *r;

	list = kzalloc(sizeof(*list), GFP_KERNEL);
	if (!list)
		return 0;

	r = &list->clip.c;
	r->left = x;
	r->top = y;
	r->width = w;
	r->height = h;

	list_add_tail(&list->list, head);
	return 1;
}

static void nuvoton_video_merge_rect(struct nuvoton_video *video,
				     struct rect_list_info *info)
{
	struct list_head *head = info->head;
	struct rect_list *list = info->list;
	struct rect_list *first = info->first;
	struct v4l2_rect *r = &list->clip.c;
	struct v4l2_rect *f = &first->clip.c;

	if (!first) {
		first = list;
		info->first = first;
		list_add_tail(&list->list, head);
		video->rect_cnt++;
	} else {
		if ((r->left ==
		      (f->left + f->width)) &&
		      r->top == f->top) {
			f->width += r->width;
			kfree(list);
		} else if ((r->top ==
			     (f->top + f->height)) &&
			    (r->left == f->left)) {
			f->height += r->height;
			kfree(list);
		} else if (((r->top > f->top) &&
			    (r->top < (f->top + f->height))) &&
			   ((r->left > f->left) &&
			    (r->left < (f->left + f->width)))) {
			kfree(list);
		} else {
			list_add_tail(&list->list, head);
			video->rect_cnt++;
			info->first = list;
		}
	}
}

static struct rect_list *
nuvoton_video_new_rect(struct nuvoton_video *video, int offset, int index)
{
	struct v4l2_bt_timings *act = &video->active_timings;
	struct rect_list *list = NULL;
	struct v4l2_rect *r;

	list = kzalloc(sizeof(*list), GFP_KERNEL);
	if (!list)
		return NULL;

	r = &list->clip.c;

	r->left = (offset << 4);
	r->top = (index >> 2);
	r->width = RECT_W;
	r->height = RECT_H;
	if ((r->left + RECT_W) > act->width)
		r->width = act->width - r->left;
	if ((r->top  + RECT_H) > act->height)
		r->height = act->height - r->top;

	return list;
}

static int nuvoton_video_find_rect(struct nuvoton_video *video,
				   struct rect_list_info *info, u32 offset)
{
	int i = info->index;

	if (offset < info->tile_perline) {
		info->list = nuvoton_video_new_rect(video, offset, i);
		if (!info->list)
			return -ENOMEM;

		nuvoton_video_merge_rect(video, info);
	}
	return 0;
}

static int nuvoton_video_build_table(struct nuvoton_video *video,
				     struct rect_list_info *info)
{
	int i = info->index;
	int j, z;

	for (j = 0 ; j < info->offset_perline ; j += 4) {
		if (nuvoton_video_read(video, VCD_DIFF_TBL + (j + i)) != 0) {
			for (z = 0 ; z < 32; z++) {
				if ((nuvoton_video_read(video, VCD_DIFF_TBL + (j + i)) >> z) &
					  0x01) {
					int ret;
					u32 offset = z + (j << 3);

					ret = nuvoton_video_find_rect(video, info, offset);
					if (ret < 0)
						return ret;
				}
			}
		}
	}
	info->index += 64;
	return info->tile_perline;
}

static int nuvoton_video_get_rect_list(struct nuvoton_video *video, int index)
{
	struct v4l2_bt_timings *act = &video->active_timings;
	struct rect_list_info info;
	int ret = 0;
	u32 mod, tile_cnt = 0;

	memset(&info, 0, sizeof(struct rect_list_info));
	info.head = &video->list[index];

	info.tile_perline = act->width >> 4;
	mod = act->width % RECT_W;
	if (mod != 0)
		info.tile_perline += 1;

	info.tile_perrow = act->height >> 4;
	mod = act->height % RECT_H;
	if (mod != 0)
		info.tile_perrow += 1;

	info.tile_size =
		info.tile_perrow * info.tile_perline;

	info.offset_perline = info.tile_perline >> 5;
	mod = info.tile_perline % 32;
	if (mod != 0)
		info.offset_perline += 1;

	info.offset_perline *= 4;

	do {
		ret = nuvoton_video_build_table(video, &info);
		if (ret < 0)
			return ret;
		tile_cnt += ret;
	} while (tile_cnt < info.tile_size);

	return ret;
}

static u8 nuvoton_video_is_mga(struct nuvoton_video *video)
{
	struct regmap *gfxi = video->gfx_regmap;
	u32 dispst;

	regmap_read(gfxi, DISPST, &dispst);
	return ((dispst & DISPST_MGAMODE) == DISPST_MGAMODE);
}

static u32 nuvoton_video_hres(struct nuvoton_video *video)
{
	struct regmap *gfxi = video->gfx_regmap;
	u32 hvcnth, hvcntl, apb_hor_res;

	regmap_read(gfxi, HVCNTH, &hvcnth);
	regmap_read(gfxi, HVCNTL, &hvcntl);

	apb_hor_res = (((hvcnth & HVCNTH_MASK) << 8)
		+ (hvcntl & HVCNTL_MASK) + 1);

	return apb_hor_res;
}

static u32 nuvoton_video_vres(struct nuvoton_video *video)
{
	struct regmap *gfxi = video->gfx_regmap;
	u32 vvcnth, vvcntl, apb_ver_res;

	regmap_read(gfxi, VVCNTH, &vvcnth);
	regmap_read(gfxi, VVCNTL, &vvcntl);

	apb_ver_res = (((vvcnth & VVCNTH_MASK) << 8)
		+ (vvcntl & VVCNTL_MASK));

	return apb_ver_res;
}

static int nuvoton_video_ready(struct nuvoton_video *video)
{
	nuvoton_video_write(video, VCD_FB_LP, 0xffffffff);
	nuvoton_video_write(video, VCD_CAP_RES, 0xffffffff);

	if ((nuvoton_video_read(video, VCD_FB_LP) != 0xfe00fe00) ||
	    (nuvoton_video_read(video, VCD_CAP_RES) != 0x7ff07ff)) {
		dev_err(video->dev, "video hw is not ready\n");
		return -ENODEV;
	}
	return 0;
}

static int
nuvoton_video_capres(struct nuvoton_video *video, u32 hor_res, u32 vert_res)
{
	u32 res = (vert_res & VCD_CAPRES_MASK)
		| ((hor_res & VCD_CAPRES_MASK) << 16);

	if (hor_res > MAX_WIDTH || vert_res > MAX_HEIGHT)
		return -EINVAL;

	nuvoton_video_write(video, VCD_CAP_RES, res);

	/* Read back the register to check that the values were valid */
	if (nuvoton_video_read(video, VCD_CAP_RES) !=  res)
		return -EINVAL;

	return 0;
}

static int nuvoton_video_reset(struct nuvoton_video *video)
{
	struct regmap *gcr = video->gcr_regmap;

	nuvoton_video_update(video, VCD_CMD, 0, VCD_CMD_RST);
	while (!(nuvoton_video_read(video, VCD_STAT) & VCD_STAT_DONE))
		continue;

	/* Active graphic Interface Reset */
	regmap_update_bits(gcr, INTCR2, INTCR2_GIRST2, INTCR2_GIRST2);

	nuvoton_video_write(video, VCD_STAT, VCD_STAT_CLEAR);

	/* Inactive graphic Interface Reset */
	regmap_update_bits(gcr, INTCR2, INTCR2_GIRST2, ~INTCR2_GIRST2);

	/* Reset VCD_HOR_AC_LAST to 0 */
	nuvoton_video_write(video, VCD_HOR_AC_LAST, 0);

	return 0;
}

static void nuvoton_video_kvm_bw(struct nuvoton_video *video, u8 bandwidth)
{
	if (!nuvoton_video_is_mga(video))
		bandwidth = 1;

	if (bandwidth)
		nuvoton_video_update(video,
				     VCD_MODE, 0, VCD_MODE_KVM_BW_SET);
	else
		nuvoton_video_update(video,
				     VCD_MODE, VCD_MODE_KVM_BW_SET, 0);
}

static u32 nuvoton_video_pclk(struct nuvoton_video *video)
{
	struct regmap *gfxi = video->gfx_regmap;
	u32 tmp, pllfbdiv, pllinotdiv, gpllfbdiv;
	u8 gpllfbdv109, gpllfbdv8, gpllindiv;
	u8 gpllst_pllotdiv1, gpllst_pllotdiv2;

	regmap_read(gfxi, GPLLST, &tmp);
	gpllfbdv109 = (tmp & GPLLFBDV109_MASK) >> GPLLFBDV109_OFFSET;
	gpllst_pllotdiv1 = tmp & GPLLST_PLLOTDIV1_MASK;
	gpllst_pllotdiv2 =
		(tmp & GPLLST_PLLOTDIV2_MASK) >> GPLLST_PLLOTDIV2_OFFSET;

	regmap_read(gfxi, GPLLINDIV, &tmp);
	gpllfbdv8 = (tmp & GPLLFBDV8_MASK) >> GPLLFBDV8_OFFSET;
	gpllindiv = (tmp & GPLLINDIV_MASK);

	regmap_read(gfxi, GPLLFBDIV, &tmp);
	gpllfbdiv = tmp & GPLLFBDIV_MASK;

	pllfbdiv = (512 * gpllfbdv109 + 256 * gpllfbdv8 + gpllfbdiv);
	pllinotdiv = (gpllindiv * gpllst_pllotdiv1 * gpllst_pllotdiv2);
	if (pllfbdiv == 0 || pllinotdiv == 0)
		return 0;

	return ((pllfbdiv * 25000) / pllinotdiv) * 1000;
}

static int nuvoton_video_get_bpp(struct nuvoton_video *video)
{
	u8 color_cnvr = ((nuvoton_video_read(video, VCD_MODE)
		& VCD_MODE_COLOR_CNVRT)
		>> VCD_MODE_COLOR_CNVRT_OFFSET);

	switch (color_cnvr) {
	case VCD_MODE_COLOR_NORM:
		return 2;
	case VCD_MODE_COLOR_222:
	case VCD_MODE_COLOR_666:
		return 1;
	case VCD_MODE_COLOR_888:
		return 4;
	}
	return 0;
}

static void
nuvoton_video_set_linepitch(struct nuvoton_video *video, u32 linebytes)
{
	/* Pitch must be a power of 2, >= linebytes,*/
	/* at least 512, and no more than 4096. */
	u32 pitch = MIN_LP;

	while ((pitch < linebytes) && (pitch < MAX_LP))
		pitch *= 2;

	nuvoton_video_write(video,
			    VCD_FB_LP, (pitch << VCD_FBB_LP_OFFSET) | pitch);
}

static u32 nuvoton_video_get_linepitch(struct nuvoton_video *video)
{
	return nuvoton_video_read(video, VCD_FB_LP) & VCD_FB_LP_MASK;
}

static u32 nuvoton_video_is_busy(struct nuvoton_video *video)
{
	return ((nuvoton_video_read(video, VCD_STAT) & VCD_STAT_BUSY));
}

static int nuvoton_video_command(struct nuvoton_video *video, u32 value)
{
	u32 cmd;

	/* Clear the status flags that could be set by this command */
	nuvoton_video_write(video, VCD_STAT, VCD_STAT_CLEAR);

	cmd = nuvoton_video_read(video, VCD_CMD) & ~VCD_CMD_OP_MASK;
	cmd |= (value << VCD_CMD_OP_OFFSET);

	nuvoton_video_write(video, VCD_CMD, cmd);
	nuvoton_video_write(video, VCD_CMD, cmd | VCD_CMD_GO);
	video->cmd = value;

	return 0;
}

static int nuvoton_video_init_reg(struct nuvoton_video *video)
{
	struct regmap *gcr = video->gcr_regmap;

	/* Selects Data Enable*/
	regmap_update_bits(gcr, INTCR, INTCR_DEHS, ~INTCR_DEHS);

	/* Enable display of KVM GFX and access to memory */
	regmap_update_bits(gcr, INTCR, INTCR_GFXIFDIS, ~INTCR_GFXIFDIS);

	/* Active Vertical/Horizontal Counters Reset */
	regmap_update_bits(gcr, INTCR2,
			   INTCR2_GIHCRST | INTCR2_GIVCRST,
			INTCR2_GIHCRST | INTCR2_GIVCRST);

	/* Select KVM GFX input */
	regmap_update_bits(gcr, MFSEL1, MFSEL1_DVH1SEL, ~MFSEL1_DVH1SEL);

	if (nuvoton_video_ready(video))
		return	-ENODEV;

	/* Reset VCD module */
	nuvoton_video_reset(video);

	/* Set the FIFO thresholds */
	nuvoton_video_write(video, VCD_FIFO, VCD_FIFO_TH);

	/* Set video mode */
	nuvoton_video_update(video, VCD_MODE, 0,
			     VCD_MODE_VCDE | VCD_MODE_CM_565 |
			   VCD_MODE_IDBC | VCD_MODE_KVM_BW_SET);

	nuvoton_video_update(video, VCD_RCHG, VCD_RCHG_TIM_PRSCL,
			     0x01 << VCD_RCHG_TIM_PRSCL_OFFSET);

	return 0;
}

static int nuvoton_video_start_frame(struct nuvoton_video *video)
{
	dma_addr_t dma_addr;
	unsigned long flags;
	struct nuvoton_video_buffer *buf;

	if (video->v4l2_input_status ||
		nuvoton_video_is_busy(video)) {
		dev_err(video->dev, "No signal/Busy; don't start frame\n");
		video->refresh = 10;
		nuvoton_video_write(video, VCD_STAT, VCD_STAT_CLEAR);
		nuvoton_video_write(video, VCD_INTE, 0);
		nuvoton_video_reset(video);
		nuvoton_video_update(video, VCD_INTE, 0, VCD_INTE_HAC_CHG_IE);
		nuvoton_video_command(video, VCD_CMD_OP_CAPTURE);
		return -EBUSY;
	}

	spin_lock_irqsave(&video->lock, flags);
	buf = list_first_entry_or_null(&video->buffers,
				       struct nuvoton_video_buffer, link);
	if (!buf) {
		spin_unlock_irqrestore(&video->lock, flags);
		dev_err(video->dev, "No buffers; don't start frame\n");
		return -EPROTO;
	}

	set_bit(VIDEO_FRAME_INPRG, &video->flags);
	dma_addr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
	spin_unlock_irqrestore(&video->lock, flags);

	nuvoton_video_update(video, VCD_INTE, 0,
			     VCD_INTE_DONE_IE | VCD_INTE_HAC_CHG_IE);

	if (video->refresh > 0) {
		nuvoton_video_command(video, VCD_CMD_OP_CAPTURE);
		video->refresh--;
	} else {
		nuvoton_video_command(video, VCD_CMD_OP_COMPARE);
	}

	return 0;
}

static void nuvoton_video_bufs_done(struct nuvoton_video *video,
				    enum vb2_buffer_state state)
{
	unsigned long flags;
	struct nuvoton_video_buffer *buf;

	spin_lock_irqsave(&video->lock, flags);
	list_for_each_entry(buf, &video->buffers, link)
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	INIT_LIST_HEAD(&video->buffers);
	spin_unlock_irqrestore(&video->lock, flags);
}

static int
nuvoton_video_copy(struct nuvoton_video *video, int index, void *addr)
{
	char *buffer = (char *)addr;
	u32 bytes = 0;
	u32 width = video->active_timings.width;
	u32 height = video->active_timings.height;
	int i;

	if (video->cmd != VCD_CMD_OP_CAPTURE) {
		video->rect_cnt = 0;
		nuvoton_video_get_rect_list(video, index);
		video->clipcount[index] = video->rect_cnt;
	} else {
		video->clipcount[index]  =
			nuvoton_video_add_rect(video, index, 0, 0, width, height);
	}

	for (i = 0 ; i < height ; i++) {
		u32 len = width * video->bytesperpixel;
		u32 src_of = i * video->bytesperline;

		memcpy(&buffer[bytes],
		       video->src.virt + src_of, len);
		bytes += len;
	}

	return bytes;
}

static irqreturn_t nuvoton_video_irq(int irq, void *arg)
{
	struct nuvoton_video *video = arg;
	u32 status = nuvoton_video_read(video, VCD_STAT);

	dev_dbg(video->dev, "vcd status 0x%x\n", status);

	if (status & VCD_STAT_HAC_CHG) {
		u32 hor_ac_tim = nuvoton_video_read(video, VCD_HOR_AC_TIM);

		video->hac_last = hor_ac_tim & VCD_HOR_AC_TIM_VALUE;

		dev_dbg(video->dev, "VCD_STAT_HAC_CHG 0x%x\n", video->hac_last);

		nuvoton_video_write(video, VCD_INTE, 0);
		nuvoton_video_write(video, VCD_STAT, VCD_STAT_CLEAR);
		nuvoton_video_write(video, VCD_HOR_AC_LAST, video->hac_last);

		set_bit(VIDEO_RES_CHANGE, &video->flags);
		clear_bit(VIDEO_FRAME_INPRG, &video->flags);

		if (video->hac_last == VCD_HOR_AC_TIM_VALUE ||
		    nuvoton_video_hres(video) == 0 ||
			nuvoton_video_vres(video) == 0) {
			nuvoton_video_reset(video);
			nuvoton_video_write(video, VCD_INTE, VCD_INTE_HAC_CHG_IE);
			nuvoton_video_command(video, VCD_CMD_OP_CAPTURE);
			return IRQ_HANDLED;
		}

		schedule_delayed_work(&video->res_work,
				RESOLUTION_CHANGE_DELAY);

		return IRQ_HANDLED;
	}

	if (status & VCD_STAT_DONE) {
		struct nuvoton_video_buffer *buf;
		u32 frame_size;

		spin_lock(&video->lock);
		buf = list_first_entry_or_null(&video->buffers,
					       struct nuvoton_video_buffer, link);
		if (buf) {
			void *addr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
			int index = buf->vb.vb2_buf.index;

			frame_size = nuvoton_video_copy(video, index, addr);
			vb2_set_plane_payload(&buf->vb.vb2_buf, 0, frame_size);

			buf->vb.vb2_buf.timestamp = ktime_get_ns();
			buf->vb.sequence = video->sequence++;
			buf->vb.field = V4L2_FIELD_NONE;
			vb2_buffer_done(&buf->vb.vb2_buf,
					VB2_BUF_STATE_DONE);
			list_del(&buf->link);
		}
		spin_unlock(&video->lock);

		nuvoton_video_write(video, VCD_STAT, VCD_STAT_CLEAR);
		nuvoton_video_update(video, VCD_INTE, VCD_INTE_DONE_IE, 0);
		clear_bit(VIDEO_FRAME_INPRG, &video->flags);

		if (test_bit(VIDEO_STREAMING, &video->flags) &&
		    buf &&
			!list_empty(&video->buffers) &&
			video->refresh > 0)
			nuvoton_video_start_frame(video);
	}

	return IRQ_HANDLED;
}

static void nuvoton_video_get_resolution(struct nuvoton_video *video)
{
	struct v4l2_bt_timings *act = &video->active_timings;
	struct v4l2_bt_timings *det = &video->detected_timings;

	video->v4l2_input_status = 0;

	det->width = nuvoton_video_hres(video);
	det->height = nuvoton_video_vres(video);

	if (act->width != det->width ||
	    act->height != det->height) {
		dev_dbg(video->dev, "resolution changed;\n");

		nuvoton_video_bufs_done(video, VB2_BUF_STATE_ERROR);

		if (nuvoton_video_hres(video) > 0 &&
		    nuvoton_video_vres(video) > 0) {
			while (nuvoton_video_vres(video) < 100 ||
			       nuvoton_video_pclk(video) == 0)
				mdelay(500);

			det->width = nuvoton_video_hres(video);
			det->height = nuvoton_video_vres(video);
			det->pixelclock = nuvoton_video_pclk(video);
		}

		video->refresh = MAX_FRAME_RATE;
	}

	if (det->width == 0 ||
	    det->height == 0) {
		det->width = MIN_WIDTH;
		det->height = MIN_HEIGHT;
		video->v4l2_input_status = V4L2_IN_ST_NO_SIGNAL;
	}

	if (test_bit(VIDEO_STREAMING, &video->flags))
		nuvoton_video_update(video, VCD_INTE, 0, VCD_INTE_HAC_CHG_IE);

	dev_dbg(video->dev, "Got resolution[%dx%d] -> [%dx%d]  status %d\n", act->width,
		act->height, det->width,
		 det->height, video->v4l2_input_status);
}

static void nuvoton_video_set_resolution(struct nuvoton_video *video)
{
	struct v4l2_bt_timings *act = &video->active_timings;

	/* Set video frame physical address*/
	nuvoton_video_write(video, VCD_FBA_ADR, video->src.dma);
	nuvoton_video_write(video, VCD_FBB_ADR, video->src.dma);

	nuvoton_video_capres(video, act->width, act->height);

	video->bytesperpixel = nuvoton_video_get_bpp(video);
	nuvoton_video_set_linepitch(video, act->width * video->bytesperpixel);

	video->bytesperline = nuvoton_video_get_linepitch(video);

	nuvoton_video_kvm_bw(video, act->pixelclock > VCD_KVM_BW_PCLK);

	nuvoton_video_reset(video);

	dev_dbg(video->dev, "VCD mode = 0x%x, %s mode\n",
		nuvoton_video_read(video, VCD_MODE),
		nuvoton_video_is_mga(video) ? "Hi Res" : "VGA");

	dev_dbg(video->dev, "Digital mode: %d x %d x %d, pixelclock %lld, bytesperline %d\n",
		act->width, act->height, video->bytesperpixel,
		act->pixelclock, video->bytesperline);

	return;
}

static int nuvoton_video_start(struct nuvoton_video *video)
{
	int rc;

	dev_dbg(video->dev, "%s\n", __func__);

	rc = nuvoton_video_init_reg(video);
	if (rc)
		return rc;

	nuvoton_video_get_resolution(video);

	video->active_timings = video->detected_timings;

	video->max_buffer_size = 0x600000;
	if (!nuvoton_video_alloc_buf(video, &video->src,
				     video->max_buffer_size))
		return -ENOMEM;

	dev_dbg(video->dev, "alloc size %d\n", video->src.size);

	nuvoton_video_set_resolution(video);

	video->pix_fmt.width = video->active_timings.width;
	video->pix_fmt.height = video->active_timings.height;
	video->pix_fmt.sizeimage = video->max_buffer_size;
	video->pix_fmt.bytesperline = video->bytesperline;
	return 0;
}

static void nuvoton_video_stop(struct nuvoton_video *video)
{
	dev_dbg(video->dev, "%s\n", __func__);

	set_bit(VIDEO_STOPPED, &video->flags);
	cancel_delayed_work_sync(&video->res_work);

	nuvoton_video_write(video, VCD_INTE, 0);
	nuvoton_video_write(video, VCD_MODE, 0);
	nuvoton_video_write(video, VCD_RCHG, 0);
	nuvoton_video_write(video, VCD_STAT, VCD_STAT_CLEAR);

	if (video->src.size)
		nuvoton_video_free_buf(video, &video->src);

	if (video->clipcount) {
		kfree(video->clipcount);
		video->clipcount = NULL;
	}

	if (video->list) {
		nuvoton_video_free_diff_table(video);
		kfree(video->list);
		video->list = NULL;
	}

	video->v4l2_input_status = V4L2_IN_ST_NO_SIGNAL;
	video->flags = 0;
}

static int nuvoton_video_querycap(struct file *file, void *fh,
				  struct v4l2_capability *cap)
{
	strscpy(cap->driver, DEVICE_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Nuvoron Video Engine", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 DEVICE_NAME);

	return 0;
}

static int nuvoton_video_enum_format(struct file *file, void *fh,
				     struct v4l2_fmtdesc *f)
{
	if (f->index > NUM_FORMATS)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_RGB565;

	return 0;
}

static int nuvoton_video_get_format(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	struct nuvoton_video *video = video_drvdata(file);

	f->fmt.pix = video->pix_fmt;

	return 0;
}

static int nuvoton_video_try_format(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	int i;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	for (i = 0; i < NUM_FORMATS; i++)
		if (f->fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565)
			return 0;

	return -EINVAL;
}

static int nuvoton_video_set_format(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	struct nuvoton_video *video = video_drvdata(file);
	int ret;

	if (vb2_is_busy(&video->queue)) {
		dev_err(video->dev, "%s device busy\n", __func__);
		return -EBUSY;
	}

	ret = nuvoton_video_try_format(file, fh, f);
	if (!ret)
		video->pix_fmt.pixelformat = f->fmt.pix.pixelformat;
	else
		return ret;

	return 0;
}

static int nuvoton_video_enum_input(struct file *file, void *fh,
				    struct v4l2_input *inp)
{
	struct nuvoton_video *video = video_drvdata(file);

	if (inp->index)
		return -EINVAL;

	strlcpy(inp->name, "Host VGA capture", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	inp->status = video->v4l2_input_status;

	return 0;
}

static int
nuvoton_video_get_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;

	return 0;
}

static int
nuvoton_video_set_input(struct file *file, void *fh, unsigned int i)
{
	if (i)
		return -EINVAL;

	return 0;
}

static int nuvoton_video_get_parm(struct file *file, void *fh,
				  struct v4l2_streamparm *a)
{
	struct nuvoton_video *video = video_drvdata(file);

	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.readbuffers = 3;
	a->parm.capture.timeperframe.numerator = 1;
	if (!video->frame_rate)
		a->parm.capture.timeperframe.denominator = MAX_FRAME_RATE;
	else
		a->parm.capture.timeperframe.denominator = video->frame_rate;

	return 0;
}

static int nuvoton_video_set_parm(struct file *file, void *fh,
				  struct v4l2_streamparm *a)
{
	unsigned int frame_rate = 0;

	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.readbuffers = 3;

	if (a->parm.capture.timeperframe.numerator)
		frame_rate = a->parm.capture.timeperframe.denominator /
			a->parm.capture.timeperframe.numerator;

	if (!frame_rate || frame_rate > MAX_FRAME_RATE) {
		frame_rate = 0;
		a->parm.capture.timeperframe.denominator = MAX_FRAME_RATE;
		a->parm.capture.timeperframe.numerator = 1;
	}

	return 0;
}

static int nuvoton_video_enum_framesizes(struct file *file, void *fh,
					 struct v4l2_frmsizeenum *fsize)
{
	struct nuvoton_video *video = video_drvdata(file);

	if (fsize->index)
		return -EINVAL;

	if (fsize->pixel_format != V4L2_PIX_FMT_RGB565)
		return -EINVAL;

	fsize->discrete.width = video->pix_fmt.width;
	fsize->discrete.height = video->pix_fmt.height;
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;

	return 0;
}

static int nuvoton_video_enum_frameintervals(struct file *file, void *fh,
					     struct v4l2_frmivalenum *fival)
{
	struct nuvoton_video *video = video_drvdata(file);

	if (fival->index)
		return -EINVAL;

	if (fival->width != video->detected_timings.width ||
	    fival->height != video->detected_timings.height)
		return -EINVAL;

	if (fival->pixel_format != V4L2_PIX_FMT_RGB565)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_CONTINUOUS;

	fival->stepwise.min.denominator = MAX_FRAME_RATE;
	fival->stepwise.min.numerator = 1;
	fival->stepwise.max.denominator = 1;
	fival->stepwise.max.numerator = 1;
	fival->stepwise.step = fival->stepwise.max;

	return 0;
}

static int nuvoton_video_get_vid_overlay(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct nuvoton_video *video = video_drvdata(file);
	struct v4l2_window *win = &fmt->fmt.win;
	struct list_head *head;
	struct rect_list *entry;

	win->clipcount = video->clipcount[video->buffer_index];

	head = &video->list[video->buffer_index];

	entry = list_first_entry_or_null(head,
					 struct rect_list,
			    list);
	if (entry) {
		struct v4l2_rect *r = &entry->clip.c;

		win->w.top = r->top;
		win->w.left = r->left;
		win->w.width = r->width;
		win->w.height = r->height;
		list_del(&entry->list);
		kfree(entry);
		video->clipcount[video->buffer_index]--;
	}

	return 0;
}

static int nuvoton_video_set_dv_timings(struct file *file, void *fh,
					struct v4l2_dv_timings *timings)
{
	struct nuvoton_video *video = video_drvdata(file);

	if (timings->bt.width == video->active_timings.width &&
	    timings->bt.height == video->active_timings.height)
		return 0;

	if (vb2_is_busy(&video->queue)) {
		dev_err(video->dev, "%s device busy\n", __func__);
		return -EBUSY;
	}

	video->active_timings = timings->bt;

	nuvoton_video_set_resolution(video);

	video->pix_fmt.width = timings->bt.width;
	video->pix_fmt.height = timings->bt.height;
	video->pix_fmt.sizeimage = video->max_buffer_size;
	video->pix_fmt.bytesperline = video->bytesperline;

	timings->type = V4L2_DV_BT_656_1120;

	return 0;
}

static int nuvoton_video_get_dv_timings(struct file *file, void *fh,
					struct v4l2_dv_timings *timings)
{
	struct nuvoton_video *video = video_drvdata(file);

	timings->type = V4L2_DV_BT_656_1120;
	timings->bt = video->active_timings;

	return 0;
}

static int nuvoton_video_query_dv_timings(struct file *file, void *fh,
					  struct v4l2_dv_timings *timings)
{
	int rc;
	struct nuvoton_video *video = video_drvdata(file);

	/*
	 * This blocks only if the driver is currently in the process of
	 * detecting a new resolution; in the event of no signal or timeout
	 * this function is woken up.
	 */
	if (file->f_flags & O_NONBLOCK) {
		if (test_bit(VIDEO_RES_CHANGE, &video->flags))
			return -EAGAIN;
	} else {
		rc = wait_event_interruptible(video->wait,
					      !test_bit(VIDEO_RES_CHANGE,
				&video->flags));
		if (rc)
			return -EINTR;
	}

	timings->type = V4L2_DV_BT_656_1120;
	timings->bt = video->detected_timings;

	return video->v4l2_input_status ? -ENOLINK : 0;
}

static int nuvoton_video_enum_dv_timings(struct file *file, void *fh,
					 struct v4l2_enum_dv_timings *timings)
{
	return v4l2_enum_dv_timings_cap(timings, &nuvoton_video_timings_cap,
				       NULL, NULL);
}

static int nuvoton_video_dv_timings_cap(struct file *file, void *fh,
					struct v4l2_dv_timings_cap *cap)
{
	*cap = nuvoton_video_timings_cap;

	return 0;
}

static int nuvoton_video_sub_event(struct v4l2_fh *fh,
				   const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	}

	return v4l2_ctrl_subscribe_event(fh, sub);
}

static const struct v4l2_ioctl_ops nuvoton_video_ioctls = {
	.vidioc_querycap = nuvoton_video_querycap,

	.vidioc_enum_fmt_vid_cap = nuvoton_video_enum_format,
	.vidioc_g_fmt_vid_cap = nuvoton_video_get_format,
	.vidioc_s_fmt_vid_cap = nuvoton_video_set_format,
	.vidioc_try_fmt_vid_cap = nuvoton_video_try_format,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_enum_input = nuvoton_video_enum_input,
	.vidioc_g_input = nuvoton_video_get_input,
	.vidioc_s_input = nuvoton_video_set_input,

	.vidioc_g_parm = nuvoton_video_get_parm,
	.vidioc_s_parm = nuvoton_video_set_parm,
	.vidioc_g_fmt_vid_overlay = nuvoton_video_get_vid_overlay,
	.vidioc_enum_framesizes = nuvoton_video_enum_framesizes,
	.vidioc_enum_frameintervals = nuvoton_video_enum_frameintervals,

	.vidioc_s_dv_timings = nuvoton_video_set_dv_timings,
	.vidioc_g_dv_timings = nuvoton_video_get_dv_timings,
	.vidioc_query_dv_timings = nuvoton_video_query_dv_timings,
	.vidioc_enum_dv_timings = nuvoton_video_enum_dv_timings,
	.vidioc_dv_timings_cap = nuvoton_video_dv_timings_cap,

	.vidioc_subscribe_event = nuvoton_video_sub_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static void nuvoton_video_resolution_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct nuvoton_video *video = container_of(dwork, struct nuvoton_video,
						  res_work);
	u32 input_status = video->v4l2_input_status;

	dev_dbg(video->dev, "resolution work; %d\n", input_status);

	/* Exit early in case no clients remain */
	if (test_bit(VIDEO_STOPPED, &video->flags))
		goto done;

	nuvoton_video_get_resolution(video);

	if (video->detected_timings.width != video->active_timings.width ||
	    video->detected_timings.height != video->active_timings.height ||
		input_status != video->v4l2_input_status) {
			static const struct v4l2_event ev = {
				.type = V4L2_EVENT_SOURCE_CHANGE,
				.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
			};

			v4l2_event_queue(&video->vdev, &ev);
	} else if (test_bit(VIDEO_STREAMING, &video->flags)) {
		/* No resolution change so just restart streaming */
		nuvoton_video_start_frame(video);
	}

done:
	clear_bit(VIDEO_RES_CHANGE, &video->flags);
	wake_up_interruptible_all(&video->wait);
}

static int nuvoton_video_open(struct file *file)
{
	int rc;
	struct nuvoton_video *video = video_drvdata(file);

	mutex_lock(&video->video_lock);

	rc = v4l2_fh_open(file);
	if (rc) {
		mutex_unlock(&video->video_lock);
		return rc;
	}

	if (v4l2_fh_is_singular_file(file))
		nuvoton_video_start(video);

	mutex_unlock(&video->video_lock);

	return 0;
}

static int nuvoton_video_release(struct file *file)
{
	int rc;
	struct nuvoton_video *video = video_drvdata(file);

	mutex_lock(&video->video_lock);

	if (v4l2_fh_is_singular_file(file))
		nuvoton_video_stop(video);

	rc = _vb2_fop_release(file, NULL);

	mutex_unlock(&video->video_lock);

	return rc;
}

static const struct v4l2_file_operations nuvoton_video_v4l2_fops = {
	.owner = THIS_MODULE,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.open = nuvoton_video_open,
	.release = nuvoton_video_release,
};

static int nuvoton_video_queue_setup(struct vb2_queue *q,
				     unsigned int *num_buffers,
				    unsigned int *num_planes,
				    unsigned int sizes[],
				    struct device *alloc_devs[])
{
	struct nuvoton_video *video = vb2_get_drv_priv(q);
	int i;

	if (*num_planes) {
		if (sizes[0] < video->max_buffer_size)
			return -EINVAL;

		return 0;
	}

	*num_planes = 1;
	sizes[0] = video->max_buffer_size;

	if (video->clipcount) {
		kfree(video->clipcount);
		video->clipcount = NULL;
	}

	video->clipcount = kcalloc(*num_buffers, sizeof(*video->clipcount), GFP_KERNEL);

	if (video->list) {
		nuvoton_video_free_diff_table(video);
		kfree(video->list);
		video->list = NULL;
	}

	video->list = (struct list_head *)kzalloc(sizeof(*video->list) * *num_buffers, GFP_KERNEL);

	for (i = 0 ; i < *num_buffers ; i++)
		INIT_LIST_HEAD(&video->list[i]);

	video->num_buffers = *num_buffers;

	return 0;
}

static int nuvoton_video_buf_prepare(struct vb2_buffer *vb)
{
	struct nuvoton_video *video = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < video->max_buffer_size)
		return -EINVAL;

	return 0;
}

static int nuvoton_video_start_streaming(struct vb2_queue *q,
					 unsigned int count)
{
	int rc;
	struct nuvoton_video *video = vb2_get_drv_priv(q);

	dev_dbg(video->dev, "%s\n", __func__);

	video->sequence = 0;
	video->refresh = MAX_FRAME_RATE;

	rc = nuvoton_video_start_frame(video);
	if (rc) {
		nuvoton_video_bufs_done(video, VB2_BUF_STATE_QUEUED);
		return rc;
	}

	set_bit(VIDEO_STREAMING, &video->flags);
	return 0;
}

static void nuvoton_video_stop_streaming(struct vb2_queue *q)
{
	int rc;
	struct nuvoton_video *video = vb2_get_drv_priv(q);

	dev_dbg(video->dev, "%s\n", __func__);

	clear_bit(VIDEO_STREAMING, &video->flags);

	rc = wait_event_timeout(video->wait,
				!test_bit(VIDEO_FRAME_INPRG, &video->flags),
				STOP_TIMEOUT);
	if (!rc)
		dev_err(video->dev, "Timed out when stopping streaming\n");

	nuvoton_video_write(video, VCD_INTE, 0);
	nuvoton_video_write(video, VCD_STAT, VCD_STAT_CLEAR);
	nuvoton_video_reset(video);

	nuvoton_video_bufs_done(video, VB2_BUF_STATE_ERROR);
}

static void nuvoton_video_buf_queue(struct vb2_buffer *vb)
{
	struct nuvoton_video *video = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct nuvoton_video_buffer *avb = to_nuvoton_video_buffer(vbuf);
	unsigned long flags;

	dev_dbg(video->dev, "%s\n", __func__);

	spin_lock_irqsave(&video->lock, flags);
	list_add_tail(&avb->link, &video->buffers);
	spin_unlock_irqrestore(&video->lock, flags);

	if (test_bit(VIDEO_STREAMING, &video->flags) &&
	    !test_bit(VIDEO_FRAME_INPRG, &video->flags) &&
		!test_bit(VIDEO_RES_CHANGE, &video->flags)) {
		dev_dbg(video->dev, "%s triger start frame\n", __func__);
		nuvoton_video_start_frame(video);
	}
}

static void nuvoton_video_buf_finish(struct vb2_buffer *vb)
{
	struct nuvoton_video *video = vb2_get_drv_priv(vb->vb2_queue);

	video->buffer_index = vb->index;
}

static const struct vb2_ops nuvoton_video_vb2_ops = {
	.queue_setup = nuvoton_video_queue_setup,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.buf_prepare = nuvoton_video_buf_prepare,
	.buf_finish = nuvoton_video_buf_finish,
	.start_streaming = nuvoton_video_start_streaming,
	.stop_streaming = nuvoton_video_stop_streaming,
	.buf_queue =  nuvoton_video_buf_queue,
};

static int nuvoton_video_setup_video(struct nuvoton_video *video)
{
	int rc;
	struct v4l2_device *v4l2_dev = &video->v4l2_dev;
	struct video_device *vdev = &video->vdev;
	struct vb2_queue *vbq = &video->queue;

	video->pix_fmt.pixelformat = V4L2_PIX_FMT_RGB565;
	video->pix_fmt.field = V4L2_FIELD_NONE;
	video->pix_fmt.colorspace = V4L2_COLORSPACE_SRGB;
	video->v4l2_input_status = V4L2_IN_ST_NO_SIGNAL;

	rc = v4l2_device_register(video->dev, v4l2_dev);
	if (rc) {
		dev_err(video->dev, "Failed to register v4l2 device\n");
		return rc;
	}

	vbq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vbq->io_modes = VB2_MMAP | VB2_READ | VB2_DMABUF;
	vbq->dev = v4l2_dev->dev;
	vbq->lock = &video->video_lock;
	vbq->ops = &nuvoton_video_vb2_ops;
	vbq->mem_ops = &vb2_dma_contig_memops;
	vbq->drv_priv = video;
	vbq->buf_struct_size = sizeof(struct nuvoton_video_buffer);
	vbq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vbq->min_buffers_needed = 3;

	rc = vb2_queue_init(vbq);
	if (rc) {
		v4l2_device_unregister(v4l2_dev);

		dev_err(video->dev, "Failed to init vb2 queue\n");
		return rc;
	}

	vdev->queue = vbq;
	vdev->fops = &nuvoton_video_v4l2_fops;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE |
		V4L2_CAP_STREAMING;
	vdev->v4l2_dev = v4l2_dev;
	strscpy(vdev->name, DEVICE_NAME, sizeof(vdev->name));
	vdev->vfl_type = VFL_TYPE_GRABBER;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->release = video_device_release_empty;
	vdev->ioctl_ops = &nuvoton_video_ioctls;
	vdev->lock = &video->video_lock;

	video_set_drvdata(vdev, video);
	rc = video_register_device(vdev, VFL_TYPE_GRABBER, 0);
	if (rc) {
		vb2_queue_release(vbq);
		v4l2_device_unregister(v4l2_dev);

		dev_err(video->dev, "Failed to register video device\n");
		return rc;
	}

	return 0;
}

static int nuvoton_video_init(struct nuvoton_video *video)
{
	int irq;
	int rc;
	struct device *dev = video->dev;

	irq = irq_of_parse_and_map(dev->of_node, 0);
	if (!irq) {
		dev_err(dev, "Unable to find VCD IRQ\n");
		return -ENODEV;
	}

	rc = devm_request_irq(dev, irq, nuvoton_video_irq, IRQF_SHARED,
			      DEVICE_NAME, video);
	if (rc < 0) {
		dev_err(dev, "Unable to request IRQ %d\n", irq);
		return rc;
	}

	rc = of_reserved_mem_device_init(dev);
	if (rc) {
		dev_err(dev, "Unable to reserve memory\n");
		return rc;
	}

	rc = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (rc) {
		dev_err(dev, "Failed to set DMA mask\n");
		of_reserved_mem_device_release(dev);
		return rc;
	}

	return 0;
}

static int nuvoton_video_probe(struct platform_device *pdev)
{
	int rc;
	struct resource *res;
	struct nuvoton_video *video = kzalloc(sizeof(*video), GFP_KERNEL);

	if (!video)
		return -ENOMEM;

	video->frame_rate = MAX_FRAME_RATE;
	video->dev = &pdev->dev;
	spin_lock_init(&video->lock);
	mutex_init(&video->video_lock);
	init_waitqueue_head(&video->wait);
	INIT_DELAYED_WORK(&video->res_work, nuvoton_video_resolution_work);
	INIT_LIST_HEAD(&video->buffers);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	video->base = devm_ioremap_resource(video->dev, res);
	if (IS_ERR(video->base))
		return PTR_ERR(video->base);

	video->gcr_regmap =
		syscon_regmap_lookup_by_compatible("nuvoton,npcm750-gcr");
	if (IS_ERR(video->gcr_regmap))
		return PTR_ERR(video->gcr_regmap);

	video->gfx_regmap =
		syscon_regmap_lookup_by_compatible("nuvoton,npcm750-gfxi");
	if (IS_ERR(video->gfx_regmap))
		return PTR_ERR(video->gfx_regmap);

	rc = nuvoton_video_init(video);
	if (rc)
		return rc;

	rc = nuvoton_video_setup_video(video);
	if (rc)
		return rc;

	dev_info(video->dev, "NPCM750 video driver probed\n");

	return 0;
}

static int nuvoton_video_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v4l2_device *v4l2_dev = dev_get_drvdata(dev);
	struct nuvoton_video *video = to_nuvoton_video(v4l2_dev);

	video_unregister_device(&video->vdev);

	vb2_queue_release(&video->queue);

	v4l2_device_unregister(v4l2_dev);

	of_reserved_mem_device_release(dev);

	return 0;
}

static const struct of_device_id nuvoton_video_match[] = {
	{
		.compatible = "nuvoton,npcm750-vcd",
	},
	{},
};

MODULE_DEVICE_TABLE(of, nuvoton_video_match)

static struct platform_driver nuvoton_video_driver = {
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = nuvoton_video_match,
	},
	.probe = nuvoton_video_probe,
	.remove = nuvoton_video_remove,
};

module_platform_driver(nuvoton_video_driver);

MODULE_AUTHOR("KWLIU<kwliu@nuvoton.com>");
MODULE_DESCRIPTION("Nuvoton video driver");
MODULE_LICENSE("GPL");
