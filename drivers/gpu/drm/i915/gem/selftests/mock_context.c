/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2016 Intel Corporation
 */

#include "i915_file_private.h"
#include "mock_context.h"
#include "selftests/mock_drm.h"
#include "selftests/mock_gtt.h"

struct i915_gem_context *
mock_context(struct drm_i915_private *i915,
	     const char *name)
{
	struct i915_gem_context *ctx;
	struct i915_gem_engines *e;
	struct intel_sseu null_sseu = {};

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	kref_init(&ctx->ref);
	INIT_LIST_HEAD(&ctx->link);
	ctx->i915 = i915;
	INIT_WORK(&ctx->release_work, i915_gem_context_release_work);

	mutex_init(&ctx->mutex);

	spin_lock_init(&ctx->stale.lock);
	INIT_LIST_HEAD(&ctx->stale.engines);

	i915_gem_context_set_persistence(ctx);

	if (name) {
		struct i915_ppgtt *ppgtt;

		strscpy(ctx->name, name, sizeof(ctx->name));

		ppgtt = mock_ppgtt(i915, name);
		if (!ppgtt)
			goto err_free;

		ctx->vm = &ppgtt->vm;
	}

	mutex_init(&ctx->engines_mutex);
	e = default_engines(ctx, null_sseu);
	if (IS_ERR(e))
		goto err_vm;
	RCU_INIT_POINTER(ctx->engines, e);

	INIT_RADIX_TREE(&ctx->handles_vma, GFP_KERNEL);
	mutex_init(&ctx->lut_mutex);

	return ctx;

err_vm:
	if (ctx->vm)
		i915_vm_put(ctx->vm);
err_free:
	kfree(ctx);
	return NULL;
}

void mock_context_close(struct i915_gem_context *ctx)
{
	context_close(ctx);
}

void mock_init_contexts(struct drm_i915_private *i915)
{
	init_contexts(&i915->gem.contexts);
}

struct i915_gem_context *
live_context(struct drm_i915_private *i915, struct file *file)
{
	struct drm_i915_file_private *fpriv = to_drm_file(file)->driver_priv;
	struct i915_gem_proto_context *pc;
	struct i915_gem_context *ctx;
	int err;
	u32 id;

	pc = proto_context_create(fpriv, i915, 0);
	if (IS_ERR(pc))
		return ERR_CAST(pc);

	ctx = i915_gem_create_context(i915, pc);
	proto_context_close(i915, pc);
	if (IS_ERR(ctx))
		return ctx;

	i915_gem_context_set_no_error_capture(ctx);

	err = xa_alloc(&fpriv->context_xa, &id, NULL, xa_limit_32b, GFP_KERNEL);
	if (err < 0)
		goto err_ctx;

	gem_context_register(ctx, fpriv, id);

	return ctx;

err_ctx:
	context_close(ctx);
	return ERR_PTR(err);
}

struct i915_gem_context *
kernel_context(struct drm_i915_private *i915,
	       struct i915_address_space *vm)
{
	struct i915_gem_context *ctx;
	struct i915_gem_proto_context *pc;

	pc = proto_context_create(NULL, i915, 0);
	if (IS_ERR(pc))
		return ERR_CAST(pc);

	if (vm) {
		if (pc->vm)
			i915_vm_put(pc->vm);
		pc->vm = i915_vm_get(vm);
	}

	ctx = i915_gem_create_context(i915, pc);
	proto_context_close(i915, pc);
	if (IS_ERR(ctx))
		return ctx;

	i915_gem_context_clear_bannable(ctx);
	i915_gem_context_set_persistence(ctx);
	i915_gem_context_set_no_error_capture(ctx);

	return ctx;
}

void kernel_context_close(struct i915_gem_context *ctx)
{
	context_close(ctx);
}
