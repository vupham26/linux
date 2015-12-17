#ifndef __NVKM_I2C_H__
#define __NVKM_I2C_H__
#include <core/subdev.h>
#include <core/event.h>

#include <subdev/bios.h>
#include <subdev/bios/i2c.h>

struct nvkm_i2c_ntfy_req {
#define NVKM_I2C_PLUG                                                      0x01
#define NVKM_I2C_UNPLUG                                                    0x02
#define NVKM_I2C_IRQ                                                       0x04
#define NVKM_I2C_DONE                                                      0x08
#define NVKM_I2C_ANY                                                       0x0f
	u8 mask;
	u8 port;
};

struct nvkm_i2c_ntfy_rep {
	u8 mask;
};

struct nvkm_i2c_bus_probe {
	struct i2c_board_info dev;
	u8 udelay; /* set to 0 to use the standard delay */
};

struct nvkm_i2c_bus {
	const struct nvkm_i2c_bus_func *func;
	struct nvkm_i2c_pad *pad;
#define NVKM_I2C_BUS_CCB(n) /* 'n' is ccb index */                           (n)
#define NVKM_I2C_BUS_EXT(n) /* 'n' is dcb external encoder type */ ((n) + 0x100)
#define NVKM_I2C_BUS_PRI /* ccb primary comm. port */                        -1
#define NVKM_I2C_BUS_SEC /* ccb secondary comm. port */                      -2
	int id;

	struct mutex mutex;
	struct list_head head;
	struct i2c_adapter i2c;
};

int nvkm_i2c_bus_acquire(struct nvkm_i2c_bus *);
void nvkm_i2c_bus_release(struct nvkm_i2c_bus *);
int nvkm_i2c_bus_probe(struct nvkm_i2c_bus *, const char *,
		       struct nvkm_i2c_bus_probe *,
		       bool (*)(struct nvkm_i2c_bus *,
			        struct i2c_board_info *, void *), void *);

struct nvkm_i2c_aux {
	const struct nvkm_i2c_aux_func *func;
	struct nvkm_i2c_pad *pad;
#define NVKM_I2C_AUX_CCB(n) /* 'n' is ccb index */                           (n)
#define NVKM_I2C_AUX_EXT(n) /* 'n' is dcb external encoder type */ ((n) + 0x100)
	int id;

	struct mutex mutex;
	struct list_head head;
	struct i2c_adapter i2c;
	void *drm_dp_aux; /* for AUX proxying on dual GPU laptops */

	u32 intr;
};

void nvkm_i2c_aux_monitor(struct nvkm_i2c_aux *, bool monitor);
int nvkm_i2c_aux_acquire(struct nvkm_i2c_aux *);
void nvkm_i2c_aux_release(struct nvkm_i2c_aux *);
int nvkm_i2c_aux_xfer(struct nvkm_i2c_aux *, bool retry, u8 type,
		      u32 addr, u8 *data, u8 size);
int nvkm_i2c_aux_lnk_ctl(struct nvkm_i2c_aux *, int link_nr, int link_bw,
			 bool enhanced_framing);

struct nvkm_i2c {
	const struct nvkm_i2c_func *func;
	struct nvkm_subdev subdev;

	struct list_head pad;
	struct list_head bus;
	struct list_head aux;

	struct nvkm_event event;
};

struct nvkm_i2c_bus *nvkm_i2c_bus_find(struct nvkm_i2c *, int);
struct nvkm_i2c_aux *nvkm_i2c_aux_find(struct nvkm_i2c *, int);

int nv04_i2c_new(struct nvkm_device *, int, struct nvkm_i2c **);
int nv4e_i2c_new(struct nvkm_device *, int, struct nvkm_i2c **);
int nv50_i2c_new(struct nvkm_device *, int, struct nvkm_i2c **);
int g94_i2c_new(struct nvkm_device *, int, struct nvkm_i2c **);
int gf117_i2c_new(struct nvkm_device *, int, struct nvkm_i2c **);
int gf119_i2c_new(struct nvkm_device *, int, struct nvkm_i2c **);
int gk104_i2c_new(struct nvkm_device *, int, struct nvkm_i2c **);
int gm204_i2c_new(struct nvkm_device *, int, struct nvkm_i2c **);

static inline int
nvkm_rdi2cr(struct i2c_adapter *adap, u8 addr, u8 reg)
{
	u8 val;
	struct i2c_msg msgs[] = {
		{ .addr = addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = &val },
	};

	int ret = i2c_transfer(adap, msgs, ARRAY_SIZE(msgs));
	if (ret != 2)
		return -EIO;

	return val;
}

static inline int
nvkm_wri2cr(struct i2c_adapter *adap, u8 addr, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msgs[] = {
		{ .addr = addr, .flags = 0, .len = 2, .buf = buf },
	};

	int ret = i2c_transfer(adap, msgs, ARRAY_SIZE(msgs));
	if (ret != 1)
		return -EIO;

	return 0;
}

/*
 * Proxying the AUX channel on dual GPU laptops:
 *
 * On read, access the AUX channel with drm_dp_dpcd_read() which will
 * automatically proxy the communication via the active GPU if necessary.
 * If that fails, fall back to accessing the AUX channel directly.
 *
 * On write, if we're the inactive GPU, compare the data to be written
 * with what's currently in the DPCD and if it's identical, skip the
 * write. If that fails, fall back to accessing the AUX channel directly.
 */

#if IS_ENABLED(CONFIG_DRM_KMS_HELPER) && !defined(__NVKM_I2C_PAD_H__)
#include <linux/vga_switcheroo.h>
#include <drm/drm_dp_helper.h>
#include <subdev/i2c/pad.h>

static inline int
drm_rdaux(struct nvkm_i2c_aux *aux, u32 addr, u8 *data, u8 size)
{
	if (!aux->drm_dp_aux ||
	    !(vga_switcheroo_handler_flags() & VGA_SWITCHEROO_NEEDS_AUX_PROXY))
		return -ENODEV;

	return drm_dp_dpcd_read(aux->drm_dp_aux, addr, data, size);

}

static inline int
nvkm_wraux_skip(struct nvkm_i2c_aux *aux, u32 addr, u8 *data, u8 size)
{
	struct drm_dp_aux *proxy_aux;
	u8 *data_rd = NULL;

	if (!aux->drm_dp_aux ||
	    !(vga_switcheroo_handler_flags() & VGA_SWITCHEROO_NEEDS_AUX_PROXY))
		return -ENODEV;

	proxy_aux = vga_switcheroo_lock_proxy_aux();
	if (!proxy_aux || proxy_aux == aux->drm_dp_aux)
		goto do_write;
	if (!(data_rd = kzalloc(size, GFP_KERNEL)))
		goto do_write;
	if (drm_dp_dpcd_read(aux->drm_dp_aux, addr, data_rd, size) != size)
		goto do_write;
	if (memcmp(data, data_rd, size) == 0) {
		nvkm_debug(&aux->pad->i2c->subdev,
			   "Skipping write to DPCD (addr=0x%x, size=%u)\n",
			   addr, size);
		vga_switcheroo_unlock_proxy_aux();
		kfree(data_rd);
		return 0;
	}

do_write:
	vga_switcheroo_unlock_proxy_aux();
	kfree(data_rd);
	return -ENODEV;
}

#else /* IS_ENABLED(CONFIG_DRM_KMS_HELPER) */

static inline int
drm_rdaux(struct nvkm_i2c_aux *aux, u32 addr, u8 *data, u8 size)
{
	return -ENODEV;
}

static inline int
nvkm_wraux_skip(struct nvkm_i2c_aux *aux, u32 addr, u8 *data, u8 size)
{
	return -ENODEV;
}

#endif /* IS_ENABLED(CONFIG_DRM_KMS_HELPER) */

static inline bool
nvkm_probe_i2c(struct i2c_adapter *adap, u8 addr)
{
	return nvkm_rdi2cr(adap, addr, 0) >= 0;
}

static inline int
nvkm_rdaux(struct nvkm_i2c_aux *aux, u32 addr, u8 *data, u8 size)
{
	int ret;

	if (drm_rdaux(aux, addr, data, size) == size)
		return 0;

	ret = nvkm_i2c_aux_acquire(aux);
	if (ret == 0) {
		ret = nvkm_i2c_aux_xfer(aux, true, 9, addr, data, size);
		nvkm_i2c_aux_release(aux);
	}
	return ret;
}

static inline int
nvkm_wraux(struct nvkm_i2c_aux *aux, u32 addr, u8 *data, u8 size)
{
	int ret;

	if (nvkm_wraux_skip(aux, addr, data, size) == 0)
		return 0;

	ret = nvkm_i2c_aux_acquire(aux);
	if (ret == 0) {
		ret = nvkm_i2c_aux_xfer(aux, true, 8, addr, data, size);
		nvkm_i2c_aux_release(aux);
	}
	return ret;
}
#endif
