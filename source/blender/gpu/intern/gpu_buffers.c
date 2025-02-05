/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Brecht Van Lommel.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/gpu/intern/gpu_buffers.c
 *  \ingroup gpu
 *
 * Mesh drawing using OpenGL VBO (Vertex Buffer Objects),
 * with fall-back to vertex arrays.
 */

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "GPU_glew.h"

#include "MEM_guardedalloc.h"

#include "BLI_bitmap.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"
#include "BLI_ghash.h"
#include "BLI_threads.h"

#include "DNA_meshdata_types.h"

#include "BKE_ccg.h"
#include "BKE_DerivedMesh.h"
#include "BKE_paint.h"
#include "BKE_pbvh.h"

#include "DNA_userdef_types.h"

#include "GPU_buffers.h"
#include "GPU_draw.h"

#include "bmesh.h"

typedef enum {
	GPU_BUFFER_VERTEX_STATE = (1 << 0),
	GPU_BUFFER_NORMAL_STATE = (1 << 1),
	GPU_BUFFER_TEXCOORD_UNIT_0_STATE = (1 << 2),
	GPU_BUFFER_TEXCOORD_UNIT_2_STATE = (1 << 3),
	GPU_BUFFER_COLOR_STATE = (1 << 4),
	GPU_BUFFER_ELEMENT_STATE = (1 << 5),
} GPUBufferState;

typedef struct {
	GLenum gl_buffer_type;
	int num_components; /* number of data components for one vertex */
} GPUBufferTypeSettings;


static int gpu_buffer_size_from_type(DerivedMesh *dm, GPUBufferType type);

const GPUBufferTypeSettings gpu_buffer_type_settings[] = {
    /* vertex */
    {GL_ARRAY_BUFFER_ARB, 3},
    /* normal */
    {GL_ARRAY_BUFFER_ARB, 4}, /* we copy 3 shorts per normal but we add a fourth for alignment */
    /* mcol */
    {GL_ARRAY_BUFFER_ARB, 3},
    /* uv */
    {GL_ARRAY_BUFFER_ARB, 2},
    /* uv for texpaint */
    {GL_ARRAY_BUFFER_ARB, 4},
    /* edge */
    {GL_ELEMENT_ARRAY_BUFFER_ARB, 2},
    /* uv edge */
    {GL_ELEMENT_ARRAY_BUFFER_ARB, 4},
    /* triangles, 1 point since we are allocating from tottriangle points, which account for all points */
    {GL_ELEMENT_ARRAY_BUFFER_ARB, 1},
    /* fast triangles */
    {GL_ELEMENT_ARRAY_BUFFER_ARB, 1},
};

#define MAX_GPU_ATTRIB_DATA 32

#define BUFFER_OFFSET(n) ((GLubyte *)NULL + (n))

/* -1 - undefined, 0 - vertex arrays, 1 - VBOs */
static GPUBufferState GLStates = 0;
static GPUAttrib attribData[MAX_GPU_ATTRIB_DATA] = { { -1, 0, 0 } };

static ThreadMutex buffer_mutex = BLI_MUTEX_INITIALIZER;

/* multires global buffer, can be used for many grids having the same grid size */
static GPUBuffer *mres_glob_buffer = NULL;
static int mres_prev_gridsize = -1;
static GLenum mres_prev_index_type = 0;
static unsigned mres_prev_totquad = 0;


/* stores recently-deleted buffers so that new buffers won't have to
 * be recreated as often
 *
 * only one instance of this pool is created, stored in
 * gpu_buffer_pool
 *
 * note that the number of buffers in the pool is usually limited to
 * MAX_FREE_GPU_BUFFERS, but this limit may be exceeded temporarily
 * when a GPUBuffer is released outside the main thread; due to OpenGL
 * restrictions it cannot be immediately released
 */
typedef struct GPUBufferPool {
	/* number of allocated buffers stored */
	int totbuf;
	/* actual allocated length of the arrays */
	int maxsize;
	GPUBuffer **buffers;
} GPUBufferPool;
#define MAX_FREE_GPU_BUFFERS 8

/* create a new GPUBufferPool */
static GPUBufferPool *gpu_buffer_pool_new(void)
{
	GPUBufferPool *pool;

	pool = MEM_callocN(sizeof(GPUBufferPool), "GPUBuffer_Pool");

	pool->maxsize = MAX_FREE_GPU_BUFFERS;
	pool->buffers = MEM_mallocN(sizeof(*pool->buffers) * pool->maxsize,
	                            "GPUBufferPool.buffers");
	return pool;
}

/* remove a GPUBuffer from the pool (does not free the GPUBuffer) */
static void gpu_buffer_pool_remove_index(GPUBufferPool *pool, int index)
{
	int i;

	if (!pool || index < 0 || index >= pool->totbuf)
		return;

	/* shift entries down, overwriting the buffer at `index' */
	for (i = index; i < pool->totbuf - 1; i++)
		pool->buffers[i] = pool->buffers[i + 1];

	/* clear the last entry */
	if (pool->totbuf > 0)
		pool->buffers[pool->totbuf - 1] = NULL;

	pool->totbuf--;
}

/* delete the last entry in the pool */
static void gpu_buffer_pool_delete_last(GPUBufferPool *pool)
{
	GPUBuffer *last;

	if (pool->totbuf <= 0)
		return;

	/* get the last entry */
	if (!(last = pool->buffers[pool->totbuf - 1]))
		return;

	/* delete the buffer's data */
	if (last->use_vbo)
		glDeleteBuffersARB(1, &last->id);
	else
		MEM_freeN(last->pointer);

	/* delete the buffer and remove from pool */
	MEM_freeN(last);
	pool->totbuf--;
	pool->buffers[pool->totbuf] = NULL;
}

/* free a GPUBufferPool; also frees the data in the pool's
 * GPUBuffers */
static void gpu_buffer_pool_free(GPUBufferPool *pool)
{
	if (!pool)
		return;
	
	while (pool->totbuf)
		gpu_buffer_pool_delete_last(pool);

	MEM_freeN(pool->buffers);
	MEM_freeN(pool);
}

static void gpu_buffer_pool_free_unused(GPUBufferPool *pool)
{
	if (!pool)
		return;

	BLI_mutex_lock(&buffer_mutex);
	
	while (pool->totbuf)
		gpu_buffer_pool_delete_last(pool);

	BLI_mutex_unlock(&buffer_mutex);
}

static GPUBufferPool *gpu_buffer_pool = NULL;
static GPUBufferPool *gpu_get_global_buffer_pool(void)
{
	/* initialize the pool */
	if (!gpu_buffer_pool)
		gpu_buffer_pool = gpu_buffer_pool_new();

	return gpu_buffer_pool;
}

void GPU_global_buffer_pool_free(void)
{
	gpu_buffer_pool_free(gpu_buffer_pool);
	gpu_buffer_pool = NULL;
}

void GPU_global_buffer_pool_free_unused(void)
{
	gpu_buffer_pool_free_unused(gpu_buffer_pool);
}

/* get a GPUBuffer of at least `size' bytes; uses one from the buffer
 * pool if possible, otherwise creates a new one
 *
 * Thread-unsafe version for internal usage only.
 */
static GPUBuffer *gpu_buffer_alloc_intern(size_t size, bool use_VBO)
{
	GPUBufferPool *pool;
	GPUBuffer *buf;
	int i, bestfit = -1;
	size_t bufsize;

	/* bad case, leads to leak of buf since buf->pointer will allocate
	 * NULL, leading to return without cleanup. In any case better detect early
	 * psy-fi */
	if (size == 0)
		return NULL;

	pool = gpu_get_global_buffer_pool();

	/* not sure if this buffer pool code has been profiled much,
	 * seems to me that the graphics driver and system memory
	 * management might do this stuff anyway. --nicholas
	 */

	/* check the global buffer pool for a recently-deleted buffer
	 * that is at least as big as the request, but not more than
	 * twice as big */
	for (i = 0; i < pool->totbuf; i++) {
		bufsize = pool->buffers[i]->size;

		/* only return a buffer that matches the VBO preference */
		if (pool->buffers[i]->use_vbo != use_VBO) {
			 continue;
		}
		
		/* check for an exact size match */
		if (bufsize == size) {
			bestfit = i;
			break;
		}
		/* smaller buffers won't fit data and buffers at least
		 * twice as big are a waste of memory */
		else if (bufsize > size && size > (bufsize / 2)) {
			/* is it closer to the required size than the
			 * last appropriate buffer found. try to save
			 * memory */
			if (bestfit == -1 || pool->buffers[bestfit]->size > bufsize) {
				bestfit = i;
			}
		}
	}

	/* if an acceptable buffer was found in the pool, remove it
	 * from the pool and return it */
	if (bestfit != -1) {
		buf = pool->buffers[bestfit];
		gpu_buffer_pool_remove_index(pool, bestfit);
		return buf;
	}

	/* no acceptable buffer found in the pool, create a new one */
	buf = MEM_callocN(sizeof(GPUBuffer), "GPUBuffer");
	buf->size = size;
	buf->use_vbo = use_VBO;

	if (use_VBO) {
		/* create a new VBO and initialize it to the requested
		 * size */
		glGenBuffersARB(1, &buf->id);
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, buf->id);
		glBufferDataARB(GL_ARRAY_BUFFER_ARB, size, NULL, GL_STATIC_DRAW_ARB);
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
	}
	else {
		static int time = 0;

		buf->pointer = MEM_mallocN(size, "GPUBuffer.pointer");

		time++;
		/* purpose of this seems to be dealing with
		 * out-of-memory errors? looks a bit iffy to me
		 * though, at least on Linux I expect malloc() would
		 * just overcommit. --nicholas */
		while (!buf->pointer && pool->totbuf > 0) {
			gpu_buffer_pool_delete_last(pool);
			buf->pointer = MEM_mallocN(size, "GPUBuffer.pointer");
		}
		if (!buf->pointer)
			return NULL;
	}

	return buf;
}

/* Same as above, but safe for threading. */
GPUBuffer *GPU_buffer_alloc(size_t size, bool force_vertex_arrays)
{
	GPUBuffer *buffer;
	bool use_VBOs = (GLEW_ARB_vertex_buffer_object) && !(U.gameflags & USER_DISABLE_VBO) && !force_vertex_arrays;

	if (size == 0) {
		/* Early out, no lock needed in this case. */
		return NULL;
	}

	BLI_mutex_lock(&buffer_mutex);
	buffer = gpu_buffer_alloc_intern(size, use_VBOs);
	BLI_mutex_unlock(&buffer_mutex);

	return buffer;
}

/* release a GPUBuffer; does not free the actual buffer or its data,
 * but rather moves it to the pool of recently-freed buffers for
 * possible re-use
 *
 * Thread-unsafe version for internal usage only.
 */
static void gpu_buffer_free_intern(GPUBuffer *buffer)
{
	GPUBufferPool *pool;
	int i;

	if (!buffer)
		return;

	pool = gpu_get_global_buffer_pool();

	/* free the last used buffer in the queue if no more space, but only
	 * if we are in the main thread. for e.g. rendering or baking it can
	 * happen that we are in other thread and can't call OpenGL, in that
	 * case cleanup will be done GPU_buffer_pool_free_unused */
	if (BLI_thread_is_main()) {
		/* in main thread, safe to decrease size of pool back
		 * down to MAX_FREE_GPU_BUFFERS */
		while (pool->totbuf >= MAX_FREE_GPU_BUFFERS)
			gpu_buffer_pool_delete_last(pool);
	}
	else {
		/* outside of main thread, can't safely delete the
		 * buffer, so increase pool size */
		if (pool->maxsize == pool->totbuf) {
			pool->maxsize += MAX_FREE_GPU_BUFFERS;
			pool->buffers = MEM_reallocN(pool->buffers,
			                             sizeof(GPUBuffer *) * pool->maxsize);
		}
	}

	/* shift pool entries up by one */
	for (i = pool->totbuf; i > 0; i--)
		pool->buffers[i] = pool->buffers[i - 1];

	/* insert the buffer into the beginning of the pool */
	pool->buffers[0] = buffer;
	pool->totbuf++;
}

/* Same as above, but safe for threading. */
void GPU_buffer_free(GPUBuffer *buffer)
{
	if (!buffer) {
		/* Early output, no need to lock in this case, */
		return;
	}

	BLI_mutex_lock(&buffer_mutex);
	gpu_buffer_free_intern(buffer);
	BLI_mutex_unlock(&buffer_mutex);
}

void GPU_buffer_multires_free(bool force)
{
	if (!mres_glob_buffer) {
		/* Early output, no need to lock in this case, */
		return;
	}

	if (force && BLI_thread_is_main()) {
		if (mres_glob_buffer) {
			if (mres_glob_buffer->id)
				glDeleteBuffersARB(1, &mres_glob_buffer->id);
			else if (mres_glob_buffer->pointer)
				MEM_freeN(mres_glob_buffer->pointer);
			MEM_freeN(mres_glob_buffer);
		}
	}
	else {
		BLI_mutex_lock(&buffer_mutex);
		gpu_buffer_free_intern(mres_glob_buffer);
		BLI_mutex_unlock(&buffer_mutex);
	}

	mres_glob_buffer = NULL;
	mres_prev_gridsize = -1;
	mres_prev_index_type = 0;
	mres_prev_totquad = 0;
}


void GPU_drawobject_free(DerivedMesh *dm)
{
	GPUDrawObject *gdo;
	int i;

	if (!dm || !(gdo = dm->drawObject))
		return;

	for (i = 0; i < gdo->totmaterial; i++) {
		if (gdo->materials[i].polys)
			MEM_freeN(gdo->materials[i].polys);
	}

	MEM_freeN(gdo->materials);
	if (gdo->vert_points)
		MEM_freeN(gdo->vert_points);
#ifdef USE_GPU_POINT_LINK
	MEM_freeN(gdo->vert_points_mem);
#endif
	GPU_buffer_free(gdo->points);
	GPU_buffer_free(gdo->normals);
	GPU_buffer_free(gdo->uv);
	GPU_buffer_free(gdo->uv_tex);
	GPU_buffer_free(gdo->colors);
	GPU_buffer_free(gdo->edges);
	GPU_buffer_free(gdo->uvedges);
	GPU_buffer_free(gdo->triangles);

	MEM_freeN(gdo);
	dm->drawObject = NULL;
}

static GPUBuffer *gpu_try_realloc(GPUBufferPool *pool, GPUBuffer *buffer, int size, bool use_VBOs)
{
	gpu_buffer_free_intern(buffer);
	gpu_buffer_pool_delete_last(pool);
	buffer = NULL;
	
	/* try freeing an entry from the pool
	 * and reallocating the buffer */
	if (pool->totbuf > 0) {
		gpu_buffer_pool_delete_last(pool);
		buffer = gpu_buffer_alloc_intern(size, use_VBOs);
	}
	
	return buffer;
}

static GPUBuffer *gpu_buffer_setup(DerivedMesh *dm, GPUDrawObject *object,
                                   int type, void *user)
{
	GPUBufferPool *pool;
	GPUBuffer *buffer;
	float *varray;
	int *mat_orig_to_new;
	int i;
	const GPUBufferTypeSettings *ts = &gpu_buffer_type_settings[type];
	GLenum target = ts->gl_buffer_type;
	int num_components = ts->num_components;
	int size = gpu_buffer_size_from_type(dm, type);
	bool use_VBOs = (GLEW_ARB_vertex_buffer_object) && !(U.gameflags & USER_DISABLE_VBO);
	GLboolean uploaded;

	pool = gpu_get_global_buffer_pool();

	BLI_mutex_lock(&buffer_mutex);

	/* alloc a GPUBuffer; fall back to legacy mode on failure */
	if (!(buffer = gpu_buffer_alloc_intern(size, use_VBOs))) {
		BLI_mutex_unlock(&buffer_mutex);
		return NULL;
	}

	mat_orig_to_new = MEM_mallocN(sizeof(*mat_orig_to_new) * dm->totmat,
	                              "GPU_buffer_setup.mat_orig_to_new");
	for (i = 0; i < object->totmaterial; i++) {
		/* for each material, the current index to copy data to */
		object->materials[i].counter = object->materials[i].start * num_components;

		/* map from original material index to new
		 * GPUBufferMaterial index */
		mat_orig_to_new[object->materials[i].mat_nr] = i;
	}

	if (use_VBOs) {
		bool success = false;

		while (!success) {
			/* bind the buffer and discard previous data,
			 * avoids stalling gpu */
			glBindBufferARB(target, buffer->id);
			glBufferDataARB(target, buffer->size, NULL, GL_STATIC_DRAW_ARB);

			/* attempt to map the buffer */
			if (!(varray = glMapBufferARB(target, GL_WRITE_ONLY_ARB))) {
				buffer = gpu_try_realloc(pool, buffer, size, true);

				/* allocation still failed; fall back
				 * to legacy mode */
				if (!buffer) {
					use_VBOs = false;
					success = true;
				}
			}
			else {
				success = true;
			}
		}

		/* check legacy fallback didn't happen */
		if (use_VBOs) {
			uploaded = GL_FALSE;
			/* attempt to upload the data to the VBO */
			while (uploaded == GL_FALSE) {
				dm->copy_gpu_data(dm, type, varray, mat_orig_to_new, user);
				/* glUnmapBuffer returns GL_FALSE if
				 * the data store is corrupted; retry
				 * in that case */
				uploaded = glUnmapBufferARB(target);
			}
		}
		glBindBufferARB(target, 0);
	}
	if (!use_VBOs) {
		/* VBO not supported, use vertex array fallback */
		if (!buffer || !buffer->pointer) {
			buffer = gpu_try_realloc(pool, buffer, size, false);
		}
		
		if (buffer) {
			varray = buffer->pointer;
			dm->copy_gpu_data(dm, type, varray, mat_orig_to_new, user);
		}
	}

	MEM_freeN(mat_orig_to_new);

	BLI_mutex_unlock(&buffer_mutex);

	return buffer;
}

/* get the GPUDrawObject buffer associated with a type */
static GPUBuffer **gpu_drawobject_buffer_from_type(GPUDrawObject *gdo, GPUBufferType type)
{
	switch (type) {
		case GPU_BUFFER_VERTEX:
			return &gdo->points;
		case GPU_BUFFER_NORMAL:
			return &gdo->normals;
		case GPU_BUFFER_COLOR:
			return &gdo->colors;
		case GPU_BUFFER_UV:
			return &gdo->uv;
		case GPU_BUFFER_UV_TEXPAINT:
			return &gdo->uv_tex;
		case GPU_BUFFER_EDGE:
			return &gdo->edges;
		case GPU_BUFFER_UVEDGE:
			return &gdo->uvedges;
		case GPU_BUFFER_TRIANGLES:
			return &gdo->triangles;
		default:
			return NULL;
	}
}

/* get the amount of space to allocate for a buffer of a particular type */
static int gpu_buffer_size_from_type(DerivedMesh *dm, GPUBufferType type)
{
	switch (type) {
		case GPU_BUFFER_VERTEX:
			return sizeof(float) * gpu_buffer_type_settings[type].num_components * (dm->drawObject->tot_triangle_point + dm->drawObject->tot_loose_point);
		case GPU_BUFFER_NORMAL:
			return sizeof(short) * gpu_buffer_type_settings[type].num_components * dm->drawObject->tot_triangle_point;
		case GPU_BUFFER_COLOR:
			return sizeof(char) * gpu_buffer_type_settings[type].num_components * dm->drawObject->tot_triangle_point;
		case GPU_BUFFER_UV:
			return sizeof(float) * gpu_buffer_type_settings[type].num_components * dm->drawObject->tot_triangle_point;
		case GPU_BUFFER_UV_TEXPAINT:
			return sizeof(float) * gpu_buffer_type_settings[type].num_components * dm->drawObject->tot_triangle_point;
		case GPU_BUFFER_EDGE:
			return sizeof(int) * gpu_buffer_type_settings[type].num_components * dm->drawObject->totedge;
		case GPU_BUFFER_UVEDGE:
			/* each face gets 3 points, 3 edges per triangle, and
			 * each edge has its own, non-shared coords, so each
			 * tri corner needs minimum of 4 floats, quads used
			 * less so here we can over allocate and assume all
			 * tris. */
			return sizeof(int) * gpu_buffer_type_settings[type].num_components * dm->drawObject->tot_triangle_point;
		case GPU_BUFFER_TRIANGLES:
			return sizeof(int) * gpu_buffer_type_settings[type].num_components * dm->drawObject->tot_triangle_point;
		default:
			return -1;
	}
}

/* call gpu_buffer_setup with settings for a particular type of buffer */
static GPUBuffer *gpu_buffer_setup_type(DerivedMesh *dm, GPUBufferType type)
{
	void *user_data = NULL;
	GPUBuffer *buf;

	/* special handling for MCol and UV buffers */
	if (type == GPU_BUFFER_COLOR) {
		if (!(user_data = DM_get_loop_data_layer(dm, dm->drawObject->colType)))
			return NULL;
	}
	else if (ELEM(type, GPU_BUFFER_UV, GPU_BUFFER_UV_TEXPAINT)) {
		if (!DM_get_loop_data_layer(dm, CD_MLOOPUV))
			return NULL;
	}

	buf = gpu_buffer_setup(dm, dm->drawObject, type, user_data);

	return buf;
}

/* get the buffer of `type', initializing the GPUDrawObject and
 * buffer if needed */
static GPUBuffer *gpu_buffer_setup_common(DerivedMesh *dm, GPUBufferType type)
{
	GPUBuffer **buf;

	if (!dm->drawObject)
		dm->drawObject = dm->gpuObjectNew(dm);

	buf = gpu_drawobject_buffer_from_type(dm->drawObject, type);
	if (!(*buf))
		*buf = gpu_buffer_setup_type(dm, type);

	return *buf;
}

void GPU_vertex_setup(DerivedMesh *dm)
{
	if (!gpu_buffer_setup_common(dm, GPU_BUFFER_VERTEX))
		return;

	glEnableClientState(GL_VERTEX_ARRAY);
	if (dm->drawObject->points->use_vbo) {
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, dm->drawObject->points->id);
		glVertexPointer(3, GL_FLOAT, 0, 0);
	}
	else {
		glVertexPointer(3, GL_FLOAT, 0, dm->drawObject->points->pointer);
	}
	
	GLStates |= GPU_BUFFER_VERTEX_STATE;
}

void GPU_normal_setup(DerivedMesh *dm)
{
	if (!gpu_buffer_setup_common(dm, GPU_BUFFER_NORMAL))
		return;

	glEnableClientState(GL_NORMAL_ARRAY);
	if (dm->drawObject->normals->use_vbo) {
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, dm->drawObject->normals->id);
		glNormalPointer(GL_SHORT, 4 * sizeof(short), 0);
	}
	else {
		glNormalPointer(GL_SHORT, 4 * sizeof(short), dm->drawObject->normals->pointer);
	}

	GLStates |= GPU_BUFFER_NORMAL_STATE;
}

void GPU_uv_setup(DerivedMesh *dm)
{
	if (!gpu_buffer_setup_common(dm, GPU_BUFFER_UV))
		return;

	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	if (dm->drawObject->uv->use_vbo) {
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, dm->drawObject->uv->id);
		glTexCoordPointer(2, GL_FLOAT, 0, 0);
	}
	else {
		glTexCoordPointer(2, GL_FLOAT, 0, dm->drawObject->uv->pointer);
	}

	GLStates |= GPU_BUFFER_TEXCOORD_UNIT_0_STATE;
}

void GPU_texpaint_uv_setup(DerivedMesh *dm)
{
	if (!gpu_buffer_setup_common(dm, GPU_BUFFER_UV_TEXPAINT))
		return;

	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	if (dm->drawObject->uv_tex->use_vbo) {
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, dm->drawObject->uv_tex->id);
		glTexCoordPointer(2, GL_FLOAT, 4 * sizeof(float), 0);
		glClientActiveTexture(GL_TEXTURE2);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(2, GL_FLOAT, 4 * sizeof(float), BUFFER_OFFSET(2 * sizeof(float)));
		glClientActiveTexture(GL_TEXTURE0);
	}
	else {
		glTexCoordPointer(2, GL_FLOAT, 4 * sizeof(float), dm->drawObject->uv_tex->pointer);
		glClientActiveTexture(GL_TEXTURE2);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(2, GL_FLOAT, 4 * sizeof(float), (char *)dm->drawObject->uv_tex->pointer + 2 * sizeof(float));
		glClientActiveTexture(GL_TEXTURE0);
	}

	GLStates |= GPU_BUFFER_TEXCOORD_UNIT_0_STATE | GPU_BUFFER_TEXCOORD_UNIT_2_STATE;
}


void GPU_color_setup(DerivedMesh *dm, int colType)
{
	if (!dm->drawObject) {
		/* XXX Not really nice, but we need a valid gpu draw object to set the colType...
		 *     Else we would have to add a new param to gpu_buffer_setup_common. */
		dm->drawObject = dm->gpuObjectNew(dm);
		dm->dirty &= ~DM_DIRTY_MCOL_UPDATE_DRAW;
		dm->drawObject->colType = colType;
	}
	/* In paint mode, dm may stay the same during stroke, however we still want to update colors!
	 * Also check in case we changed color type (i.e. which MCol cdlayer we use). */
	else if ((dm->dirty & DM_DIRTY_MCOL_UPDATE_DRAW) || (colType != dm->drawObject->colType)) {
		GPUBuffer **buf = gpu_drawobject_buffer_from_type(dm->drawObject, GPU_BUFFER_COLOR);
		/* XXX Freeing this buffer is a bit stupid, as geometry has not changed, size should remain the same.
		 *     Not sure though it would be worth defining a sort of gpu_buffer_update func - nor whether
		 *     it is even possible ! */
		GPU_buffer_free(*buf);
		*buf = NULL;
		dm->dirty &= ~DM_DIRTY_MCOL_UPDATE_DRAW;
		dm->drawObject->colType = colType;
	}

	if (!gpu_buffer_setup_common(dm, GPU_BUFFER_COLOR))
		return;

	glEnableClientState(GL_COLOR_ARRAY);
	if (dm->drawObject->colors->use_vbo) {
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, dm->drawObject->colors->id);
		glColorPointer(3, GL_UNSIGNED_BYTE, 0, 0);
	}
	else {
		glColorPointer(3, GL_UNSIGNED_BYTE, 0, dm->drawObject->colors->pointer);
	}

	GLStates |= GPU_BUFFER_COLOR_STATE;
}

void GPU_buffer_bind_as_color(GPUBuffer *buffer)
{
	glEnableClientState(GL_COLOR_ARRAY);
	if (buffer->use_vbo) {
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, buffer->id);
		glColorPointer(4, GL_UNSIGNED_BYTE, 0, 0);
	}
	else {
		glColorPointer(4, GL_UNSIGNED_BYTE, 0, buffer->pointer);
	}

	GLStates |= GPU_BUFFER_COLOR_STATE;
}


void GPU_edge_setup(DerivedMesh *dm)
{
	if (!gpu_buffer_setup_common(dm, GPU_BUFFER_EDGE))
		return;

	if (!gpu_buffer_setup_common(dm, GPU_BUFFER_VERTEX))
		return;

	glEnableClientState(GL_VERTEX_ARRAY);
	if (dm->drawObject->points->use_vbo) {
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, dm->drawObject->points->id);
		glVertexPointer(3, GL_FLOAT, 0, 0);
	}
	else {
		glVertexPointer(3, GL_FLOAT, 0, dm->drawObject->points->pointer);
	}
	
	GLStates |= GPU_BUFFER_VERTEX_STATE;

	if (dm->drawObject->edges->use_vbo)
		glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, dm->drawObject->edges->id);

	GLStates |= GPU_BUFFER_ELEMENT_STATE;
}

void GPU_uvedge_setup(DerivedMesh *dm)
{
	if (!gpu_buffer_setup_common(dm, GPU_BUFFER_UVEDGE))
		return;

	glEnableClientState(GL_VERTEX_ARRAY);
	if (dm->drawObject->uvedges->use_vbo) {
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, dm->drawObject->uvedges->id);
		glVertexPointer(2, GL_FLOAT, 0, 0);
	}
	else {
		glVertexPointer(2, GL_FLOAT, 0, dm->drawObject->uvedges->pointer);
	}
	
	GLStates |= GPU_BUFFER_VERTEX_STATE;
}

void GPU_triangle_setup(struct DerivedMesh *dm)
{
	if (!gpu_buffer_setup_common(dm, GPU_BUFFER_TRIANGLES))
		return;

	if (dm->drawObject->triangles->use_vbo) {
		glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, dm->drawObject->triangles->id);
	}

	GLStates |= GPU_BUFFER_ELEMENT_STATE;
}

static int GPU_typesize(int type)
{
	switch (type) {
		case GL_FLOAT:
			return sizeof(float);
		case GL_INT:
			return sizeof(int);
		case GL_UNSIGNED_INT:
			return sizeof(unsigned int);
		case GL_BYTE:
			return sizeof(char);
		case GL_UNSIGNED_BYTE:
			return sizeof(unsigned char);
		default:
			return 0;
	}
}

int GPU_attrib_element_size(GPUAttrib data[], int numdata)
{
	int i, elementsize = 0;

	for (i = 0; i < numdata; i++) {
		int typesize = GPU_typesize(data[i].type);
		if (typesize != 0)
			elementsize += typesize * data[i].size;
	}
	return elementsize;
}

void GPU_interleaved_attrib_setup(GPUBuffer *buffer, GPUAttrib data[], int numdata, int element_size)
{
	int i;
	int elementsize;
	intptr_t offset = 0;
	char *basep;

	for (i = 0; i < MAX_GPU_ATTRIB_DATA; i++) {
		if (attribData[i].index != -1) {
			glDisableVertexAttribArrayARB(attribData[i].index);
		}
		else
			break;
	}
	if (element_size == 0)
		elementsize = GPU_attrib_element_size(data, numdata);
	else
		elementsize = element_size;

	if (buffer->use_vbo) {
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, buffer->id);
		basep = NULL;
	}
	else {
		basep = buffer->pointer;
	}
	
	for (i = 0; i < numdata; i++) {
		glEnableVertexAttribArrayARB(data[i].index);
		glVertexAttribPointerARB(data[i].index, data[i].size, data[i].type,
		                         GL_FALSE, elementsize, (void *)(basep + offset));
		offset += data[i].size * GPU_typesize(data[i].type);
		
		attribData[i].index = data[i].index;
		attribData[i].size = data[i].size;
		attribData[i].type = data[i].type;
	}
	
	attribData[numdata].index = -1;	
}

void GPU_interleaved_attrib_unbind(void)
{
	int i;
	for (i = 0; i < MAX_GPU_ATTRIB_DATA; i++) {
		if (attribData[i].index != -1) {
			glDisableVertexAttribArrayARB(attribData[i].index);
		}
		else
			break;
	}
	attribData[0].index = -1;
}

void GPU_buffer_unbind(void)
{
	int i;

	if (GLStates & GPU_BUFFER_VERTEX_STATE)
		glDisableClientState(GL_VERTEX_ARRAY);
	if (GLStates & GPU_BUFFER_NORMAL_STATE)
		glDisableClientState(GL_NORMAL_ARRAY);
	if (GLStates & GPU_BUFFER_TEXCOORD_UNIT_0_STATE)
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	if (GLStates & GPU_BUFFER_TEXCOORD_UNIT_2_STATE) {
		glClientActiveTexture(GL_TEXTURE2);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glClientActiveTexture(GL_TEXTURE0);
	}
	if (GLStates & GPU_BUFFER_COLOR_STATE)
		glDisableClientState(GL_COLOR_ARRAY);
	if (GLStates & GPU_BUFFER_ELEMENT_STATE) {
		/* not guaranteed we used VBOs but in that case it's just a no-op */
		if (GLEW_ARB_vertex_buffer_object) {
			glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
		}
	}
	GLStates &= ~(GPU_BUFFER_VERTEX_STATE | GPU_BUFFER_NORMAL_STATE |
	              GPU_BUFFER_TEXCOORD_UNIT_0_STATE | GPU_BUFFER_TEXCOORD_UNIT_2_STATE |
	              GPU_BUFFER_COLOR_STATE | GPU_BUFFER_ELEMENT_STATE);

	for (i = 0; i < MAX_GPU_ATTRIB_DATA; i++) {
		if (attribData[i].index != -1) {
			glDisableVertexAttribArrayARB(attribData[i].index);
		}
		else
			break;
	}
	attribData[0].index = -1;

	/* not guaranteed we used VBOs but in that case it's just a no-op */
	if (GLEW_ARB_vertex_buffer_object)
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
}

void GPU_color_switch(int mode)
{
	if (mode) {
		if (!(GLStates & GPU_BUFFER_COLOR_STATE))
			glEnableClientState(GL_COLOR_ARRAY);
		GLStates |= GPU_BUFFER_COLOR_STATE;
	}
	else {
		if (GLStates & GPU_BUFFER_COLOR_STATE)
			glDisableClientState(GL_COLOR_ARRAY);
		GLStates &= ~GPU_BUFFER_COLOR_STATE;
	}
}

static int gpu_binding_type_gl[] =
{
	GL_ARRAY_BUFFER_ARB,
	GL_ELEMENT_ARRAY_BUFFER_ARB
};

void *GPU_buffer_lock(GPUBuffer *buffer, GPUBindingType binding)
{
	float *varray;

	if (!buffer)
		return 0;

	if (buffer->use_vbo) {
		int bindtypegl = gpu_binding_type_gl[binding];
		glBindBufferARB(bindtypegl, buffer->id);
		varray = glMapBufferARB(bindtypegl, GL_WRITE_ONLY_ARB);
		return varray;
	}
	else {
		return buffer->pointer;
	}
}

void *GPU_buffer_lock_stream(GPUBuffer *buffer, GPUBindingType binding)
{
	float *varray;

	if (!buffer)
		return 0;

	if (buffer->use_vbo) {
		int bindtypegl = gpu_binding_type_gl[binding];
		glBindBufferARB(bindtypegl, buffer->id);
		/* discard previous data, avoid stalling gpu */
		glBufferDataARB(bindtypegl, buffer->size, 0, GL_STREAM_DRAW_ARB);
		varray = glMapBufferARB(bindtypegl, GL_WRITE_ONLY_ARB);
		return varray;
	}
	else {
		return buffer->pointer;
	}
}

void GPU_buffer_unlock(GPUBuffer *buffer, GPUBindingType binding)
{
	if (buffer->use_vbo) {
		int bindtypegl = gpu_binding_type_gl[binding];
		/* note: this operation can fail, could return
		 * an error code from this function? */
		glUnmapBufferARB(bindtypegl);
		glBindBufferARB(bindtypegl, 0);
	}
}

void GPU_buffer_bind(GPUBuffer *buffer, GPUBindingType binding)
{
	if (buffer->use_vbo) {
		int bindtypegl = gpu_binding_type_gl[binding];
		glBindBufferARB(bindtypegl, buffer->id);
	}
}

/* used for drawing edges */
void GPU_buffer_draw_elements(GPUBuffer *elements, unsigned int mode, int start, int count)
{
	glDrawElements(mode, count, GL_UNSIGNED_INT,
	               (elements->use_vbo ?
	                (void *)(start * sizeof(unsigned int)) :
	                ((int *)elements->pointer) + start));
}


/* XXX: the rest of the code in this file is used for optimized PBVH
 * drawing and doesn't interact at all with the buffer code above */

/* Convenience struct for building the VBO. */
typedef struct {
	float co[3];
	short no[3];

	/* inserting this to align the 'color' field to a four-byte
	 * boundary; drastically increases viewport performance on my
	 * drivers (Gallium/Radeon) --nicholasbishop */
	char pad[2];
	
	unsigned char color[3];
} VertexBufferFormat;

struct GPU_PBVH_Buffers {
	/* opengl buffer handles */
	GPUBuffer *vert_buf, *index_buf, *index_buf_fast;
	GLenum index_type;

	/* mesh pointers in case buffer allocation fails */
	const MPoly *mpoly;
	const MLoop *mloop;
	const MLoopTri *looptri;
	const MVert *mvert;

	const int *face_indices;
	int        face_indices_len;
	const float *vmask;

	/* grid pointers */
	CCGKey gridkey;
	CCGElem **grids;
	const DMFlagMat *grid_flag_mats;
	BLI_bitmap * const *grid_hidden;
	const int *grid_indices;
	int totgrid;
	int has_hidden;

	int use_bmesh;

	unsigned int tot_tri, tot_quad;

	/* The PBVH ensures that either all faces in the node are
	 * smooth-shaded or all faces are flat-shaded */
	int smooth;

	bool show_diffuse_color;
	bool use_matcaps;
	float diffuse_color[4];
};

typedef enum {
	VBO_ENABLED,
	VBO_DISABLED
} VBO_State;

static void gpu_colors_enable(VBO_State vbo_state)
{
	glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
	glEnable(GL_COLOR_MATERIAL);
	if (vbo_state == VBO_ENABLED)
		glEnableClientState(GL_COLOR_ARRAY);
}

static void gpu_colors_disable(VBO_State vbo_state)
{
	glDisable(GL_COLOR_MATERIAL);
	if (vbo_state == VBO_ENABLED)
		glDisableClientState(GL_COLOR_ARRAY);
}

static float gpu_color_from_mask(float mask)
{
	return 1.0f - mask * 0.75f;
}

static void gpu_color_from_mask_copy(float mask, const float diffuse_color[4], unsigned char out[3])
{
	float mask_color;

	mask_color = gpu_color_from_mask(mask) * 255.0f;

	out[0] = diffuse_color[0] * mask_color;
	out[1] = diffuse_color[1] * mask_color;
	out[2] = diffuse_color[2] * mask_color;
}

static void gpu_color_from_mask_quad_copy(const CCGKey *key,
                                          CCGElem *a, CCGElem *b,
                                          CCGElem *c, CCGElem *d,
                                          const float *diffuse_color,
                                          unsigned char out[3])
{
	float mask_color =
	    gpu_color_from_mask((*CCG_elem_mask(key, a) +
	                         *CCG_elem_mask(key, b) +
	                         *CCG_elem_mask(key, c) +
	                         *CCG_elem_mask(key, d)) * 0.25f) * 255.0f;

	out[0] = diffuse_color[0] * mask_color;
	out[1] = diffuse_color[1] * mask_color;
	out[2] = diffuse_color[2] * mask_color;
}

void GPU_update_mesh_pbvh_buffers(
        GPU_PBVH_Buffers *buffers, const MVert *mvert,
        const int *vert_indices, int totvert, const float *vmask,
        const int (*face_vert_indices)[4], bool show_diffuse_color)
{
	VertexBufferFormat *vert_data;
	int i, j;

	buffers->vmask = vmask;
	buffers->show_diffuse_color = show_diffuse_color;
	buffers->use_matcaps = GPU_material_use_matcaps_get();

	{
		int totelem = (buffers->smooth ? totvert : (buffers->tot_tri * 3));
		float diffuse_color[4] = {0.8f, 0.8f, 0.8f, 0.8f};

		if (buffers->use_matcaps)
			diffuse_color[0] = diffuse_color[1] = diffuse_color[2] = 1.0;
		else if (show_diffuse_color) {
			const MLoopTri *lt = &buffers->looptri[buffers->face_indices[0]];
			const MPoly *mp = &buffers->mpoly[lt->poly];

			GPU_material_diffuse_get(mp->mat_nr + 1, diffuse_color);
		}

		copy_v4_v4(buffers->diffuse_color, diffuse_color);

		/* Build VBO */
		if (buffers->vert_buf)
			GPU_buffer_free(buffers->vert_buf);
		buffers->vert_buf = GPU_buffer_alloc(sizeof(VertexBufferFormat) * totelem, false);
		vert_data = GPU_buffer_lock(buffers->vert_buf, GPU_BINDING_ARRAY);

		if (vert_data) {
			/* Vertex data is shared if smooth-shaded, but separate
			 * copies are made for flat shading because normals
			 * shouldn't be shared. */
			if (buffers->smooth) {
				for (i = 0; i < totvert; ++i) {
					const MVert *v = &mvert[vert_indices[i]];
					VertexBufferFormat *out = vert_data + i;

					copy_v3_v3(out->co, v->co);
					memcpy(out->no, v->no, sizeof(short) * 3);
				}

#define UPDATE_VERTEX(face, vertex, index, diffuse_color) \
				{ \
					VertexBufferFormat *out = vert_data + face_vert_indices[face][index]; \
					if (vmask) \
						gpu_color_from_mask_copy(vmask[vertex], diffuse_color, out->color); \
					else \
						rgb_float_to_uchar(out->color, diffuse_color); \
				} (void)0

				for (i = 0; i < buffers->face_indices_len; i++) {
					const MLoopTri *lt = &buffers->looptri[buffers->face_indices[i]];
					const unsigned int vtri[3] = {
					    buffers->mloop[lt->tri[0]].v,
					    buffers->mloop[lt->tri[1]].v,
					    buffers->mloop[lt->tri[2]].v,
					};

					UPDATE_VERTEX(i, vtri[0], 0, diffuse_color);
					UPDATE_VERTEX(i, vtri[1], 1, diffuse_color);
					UPDATE_VERTEX(i, vtri[2], 2, diffuse_color);
				}
#undef UPDATE_VERTEX
			}
			else {
				for (i = 0; i < buffers->face_indices_len; ++i) {
					const MLoopTri *lt = &buffers->looptri[buffers->face_indices[i]];
					const unsigned int vtri[3] = {
					    buffers->mloop[lt->tri[0]].v,
					    buffers->mloop[lt->tri[1]].v,
					    buffers->mloop[lt->tri[2]].v,
					};
					float fno[3];
					short no[3];

					float fmask;

					if (paint_is_face_hidden(lt, mvert, buffers->mloop))
						continue;

					/* Face normal and mask */
					normal_tri_v3(fno,
					              mvert[vtri[0]].co,
					              mvert[vtri[1]].co,
					              mvert[vtri[2]].co);
					if (vmask) {
						fmask = (vmask[vtri[0]] +
						         vmask[vtri[1]] +
						         vmask[vtri[2]]) / 3.0f;
					}
					normal_float_to_short_v3(no, fno);

					for (j = 0; j < 3; j++) {
						const MVert *v = &mvert[vtri[j]];
						VertexBufferFormat *out = vert_data;

						copy_v3_v3(out->co, v->co);
						copy_v3_v3_short(out->no, no);

						if (vmask)
							gpu_color_from_mask_copy(fmask, diffuse_color, out->color);
						else
							rgb_float_to_uchar(out->color, diffuse_color);

						vert_data++;
					}
				}
			}

			GPU_buffer_unlock(buffers->vert_buf, GPU_BINDING_ARRAY);
		}
		else {
			GPU_buffer_free(buffers->vert_buf);
			buffers->vert_buf = NULL;
		}

		glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
	}

	buffers->mvert = mvert;
}

GPU_PBVH_Buffers *GPU_build_mesh_pbvh_buffers(
        const int (*face_vert_indices)[4],
        const MPoly *mpoly, const MLoop *mloop, const MLoopTri *looptri,
        const MVert *mvert,
        const int *face_indices,
        const int  face_indices_len)
{
	GPU_PBVH_Buffers *buffers;
	unsigned short *tri_data;
	int i, j, tottri;

	buffers = MEM_callocN(sizeof(GPU_PBVH_Buffers), "GPU_Buffers");
	buffers->index_type = GL_UNSIGNED_SHORT;
	buffers->smooth = mpoly[face_indices[0]].flag & ME_SMOOTH;

	buffers->show_diffuse_color = false;
	buffers->use_matcaps = false;

	/* Count the number of visible triangles */
	for (i = 0, tottri = 0; i < face_indices_len; ++i) {
		const MLoopTri *lt = &looptri[face_indices[i]];
		if (!paint_is_face_hidden(lt, mvert, mloop))
			tottri++;
	}

	if (tottri == 0) {
		buffers->tot_tri = 0;

		buffers->mpoly = mpoly;
		buffers->mloop = mloop;
		buffers->looptri = looptri;
		buffers->face_indices = face_indices;
		buffers->face_indices_len = 0;

		return buffers;
	}

	/* An element index buffer is used for smooth shading, but flat
	 * shading requires separate vertex normals so an index buffer is
	 * can't be used there. */
	if (buffers->smooth)
		buffers->index_buf = GPU_buffer_alloc(sizeof(unsigned short) * tottri * 3, false);

	if (buffers->index_buf) {
		/* Fill the triangle buffer */
		tri_data = GPU_buffer_lock(buffers->index_buf, GPU_BINDING_INDEX);
		if (tri_data) {
			for (i = 0; i < face_indices_len; ++i) {
				const MLoopTri *lt = &looptri[face_indices[i]];

				/* Skip hidden faces */
				if (paint_is_face_hidden(lt, mvert, mloop))
					continue;

				for (j = 0; j < 3; ++j) {
					*tri_data = face_vert_indices[i][j];
					tri_data++;
				}
			}
			GPU_buffer_unlock(buffers->index_buf, GPU_BINDING_INDEX);
		}
		else {
			GPU_buffer_free(buffers->index_buf);
			buffers->index_buf = NULL;
		}

		glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
	}

	buffers->tot_tri = tottri;

	buffers->mpoly = mpoly;
	buffers->mloop = mloop;
	buffers->looptri = looptri;

	buffers->face_indices = face_indices;
	buffers->face_indices_len = face_indices_len;

	return buffers;
}

void GPU_update_grid_pbvh_buffers(GPU_PBVH_Buffers *buffers, CCGElem **grids,
                                  const DMFlagMat *grid_flag_mats, int *grid_indices,
                                  int totgrid, const CCGKey *key, bool show_diffuse_color)
{
	VertexBufferFormat *vert_data;
	int i, j, k, x, y;

	buffers->show_diffuse_color = show_diffuse_color;
	buffers->use_matcaps = GPU_material_use_matcaps_get();

	/* Build VBO */
	if (buffers->vert_buf) {
		int smooth = grid_flag_mats[grid_indices[0]].flag & ME_SMOOTH;
		const int has_mask = key->has_mask;
		float diffuse_color[4] = {0.8f, 0.8f, 0.8f, 1.0f};

		if (buffers->use_matcaps)
			diffuse_color[0] = diffuse_color[1] = diffuse_color[2] = 1.0;
		else if (show_diffuse_color) {
			const DMFlagMat *flags = &grid_flag_mats[grid_indices[0]];

			GPU_material_diffuse_get(flags->mat_nr + 1, diffuse_color);
		}

		copy_v4_v4(buffers->diffuse_color, diffuse_color);

		vert_data = GPU_buffer_lock_stream(buffers->vert_buf, GPU_BINDING_ARRAY);
		if (vert_data) {
			for (i = 0; i < totgrid; ++i) {
				VertexBufferFormat *vd = vert_data;
				CCGElem *grid = grids[grid_indices[i]];

				for (y = 0; y < key->grid_size; y++) {
					for (x = 0; x < key->grid_size; x++) {
						CCGElem *elem = CCG_grid_elem(key, grid, x, y);
						
						copy_v3_v3(vd->co, CCG_elem_co(key, elem));
						if (smooth) {
							normal_float_to_short_v3(vd->no, CCG_elem_no(key, elem));

							if (has_mask) {
								gpu_color_from_mask_copy(*CCG_elem_mask(key, elem),
								                         diffuse_color, vd->color);
							}
						}
						vd++;
					}
				}
				
				if (!smooth) {
					/* for flat shading, recalc normals and set the last vertex of
					 * each triangle in the index buffer to have the flat normal as
					 * that is what opengl will use */
					for (j = 0; j < key->grid_size - 1; j++) {
						for (k = 0; k < key->grid_size - 1; k++) {
							CCGElem *elems[4] = {
								CCG_grid_elem(key, grid, k, j + 1),
								CCG_grid_elem(key, grid, k + 1, j + 1),
								CCG_grid_elem(key, grid, k + 1, j),
								CCG_grid_elem(key, grid, k, j)
							};
							float fno[3];

							normal_quad_v3(fno,
							               CCG_elem_co(key, elems[0]),
							               CCG_elem_co(key, elems[1]),
							               CCG_elem_co(key, elems[2]),
							               CCG_elem_co(key, elems[3]));

							vd = vert_data + (j + 1) * key->grid_size + k;
							normal_float_to_short_v3(vd->no, fno);

							if (has_mask) {
								gpu_color_from_mask_quad_copy(key,
								                              elems[0],
								                              elems[1],
								                              elems[2],
								                              elems[3],
								                              diffuse_color,
								                              vd->color);
							}
						}
					}
				}

				vert_data += key->grid_area;
			}

			GPU_buffer_unlock(buffers->vert_buf, GPU_BINDING_ARRAY);
		}
		else {
			GPU_buffer_free(buffers->vert_buf);
			buffers->vert_buf = NULL;
		}
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
	}

	buffers->grids = grids;
	buffers->grid_indices = grid_indices;
	buffers->totgrid = totgrid;
	buffers->grid_flag_mats = grid_flag_mats;
	buffers->gridkey = *key;

	buffers->smooth = grid_flag_mats[grid_indices[0]].flag & ME_SMOOTH;

	//printf("node updated %p\n", buffers);
}

/* Build the element array buffer of grid indices using either
 * unsigned shorts or unsigned ints. */
#define FILL_QUAD_BUFFER(type_, tot_quad_, buffer_)                     \
    {                                                                   \
        type_ *tri_data;                                                \
        int offset = 0;                                                 \
        int i, j, k;                                                    \
        buffer_ = GPU_buffer_alloc(sizeof(type_) * (tot_quad_) * 6,     \
                                  false);                               \
                                                                        \
        /* Fill the buffer */                                           \
        tri_data = GPU_buffer_lock(buffer_, GPU_BINDING_INDEX);         \
        if (tri_data) {                                                 \
            for (i = 0; i < totgrid; ++i) {                             \
                BLI_bitmap *gh = NULL;                                  \
                if (grid_hidden)                                        \
                    gh = grid_hidden[(grid_indices)[i]];                \
                                                                        \
                for (j = 0; j < gridsize - 1; ++j) {                    \
                    for (k = 0; k < gridsize - 1; ++k) {                \
                        /* Skip hidden grid face */                     \
                        if (gh &&                                       \
                            paint_is_grid_face_hidden(gh,               \
                                                      gridsize, k, j))  \
                            continue;                                    \
                                                                          \
                        *(tri_data++) = offset + j * gridsize + k + 1;     \
                        *(tri_data++) = offset + j * gridsize + k;          \
                        *(tri_data++) = offset + (j + 1) * gridsize + k;     \
                                                                             \
                        *(tri_data++) = offset + (j + 1) * gridsize + k + 1; \
                        *(tri_data++) = offset + j * gridsize + k + 1;       \
                        *(tri_data++) = offset + (j + 1) * gridsize + k;    \
                    }                                                      \
                }                                                         \
                                                                         \
                offset += gridsize * gridsize;                          \
            }                                                           \
            GPU_buffer_unlock(buffer_, GPU_BINDING_INDEX);                         \
        }                                                               \
        else {                                                          \
            GPU_buffer_free(buffer_);                                   \
            (buffer_) = NULL;                                           \
        }                                                               \
    } (void)0
/* end FILL_QUAD_BUFFER */

static GPUBuffer *gpu_get_grid_buffer(int gridsize, GLenum *index_type, unsigned *totquad)
{
	/* used in the FILL_QUAD_BUFFER macro */
	BLI_bitmap * const *grid_hidden = NULL;
	const int *grid_indices = NULL;
	int totgrid = 1;

	/* VBO is already built */
	if (mres_glob_buffer && mres_prev_gridsize == gridsize) {
		*index_type = mres_prev_index_type;
		*totquad = mres_prev_totquad;
		return mres_glob_buffer;
	}
	/* we can't reuse old, delete the existing buffer */
	else if (mres_glob_buffer) {
		GPU_buffer_free(mres_glob_buffer);
	}

	/* Build new VBO */
	*totquad = (gridsize - 1) * (gridsize - 1);

	if (gridsize * gridsize < USHRT_MAX) {
		*index_type = GL_UNSIGNED_SHORT;
		FILL_QUAD_BUFFER(unsigned short, *totquad, mres_glob_buffer);
	}
	else {
		*index_type = GL_UNSIGNED_INT;
		FILL_QUAD_BUFFER(unsigned int, *totquad, mres_glob_buffer);
	}

	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);

	mres_prev_gridsize = gridsize;
	mres_prev_index_type = *index_type;
	mres_prev_totquad = *totquad;
	return mres_glob_buffer;
}

#define FILL_FAST_BUFFER(type_) \
{ \
    type_ *buffer; \
    buffers->index_buf_fast = GPU_buffer_alloc(sizeof(type_) * 6 * totgrid, false); \
    buffer = GPU_buffer_lock(buffers->index_buf_fast, GPU_BINDING_INDEX); \
    if (buffer) { \
        int i; \
        for (i = 0; i < totgrid; i++) { \
            int currentquad = i * 6; \
            buffer[currentquad] = i * gridsize * gridsize; \
            buffer[currentquad + 1] = i * gridsize * gridsize + gridsize - 1; \
            buffer[currentquad + 2] = (i + 1) * gridsize * gridsize - gridsize; \
            buffer[currentquad + 3] = (i + 1) * gridsize * gridsize - 1; \
            buffer[currentquad + 4] = i * gridsize * gridsize + gridsize - 1; \
            buffer[currentquad + 5] = (i + 1) * gridsize * gridsize - gridsize; \
        } \
        GPU_buffer_unlock(buffers->index_buf_fast, GPU_BINDING_INDEX); \
	} \
	else { \
        GPU_buffer_free(buffers->index_buf_fast); \
        buffers->index_buf_fast = NULL; \
    } \
} (void)0

GPU_PBVH_Buffers *GPU_build_grid_pbvh_buffers(int *grid_indices, int totgrid,
                                              BLI_bitmap **grid_hidden, int gridsize, const CCGKey *key)
{
	GPU_PBVH_Buffers *buffers;
	int totquad;
	int fully_visible_totquad = (gridsize - 1) * (gridsize - 1) * totgrid;

	buffers = MEM_callocN(sizeof(GPU_PBVH_Buffers), "GPU_Buffers");
	buffers->grid_hidden = grid_hidden;
	buffers->totgrid = totgrid;

	buffers->show_diffuse_color = false;
	buffers->use_matcaps = false;

	/* Count the number of quads */
	totquad = BKE_pbvh_count_grid_quads(grid_hidden, grid_indices, totgrid, gridsize);

	/* totally hidden node, return here to avoid BufferData with zero below. */
	if (totquad == 0)
		return buffers;

	/* create and fill indices of the fast buffer too */
	if (totgrid * gridsize * gridsize < USHRT_MAX) {
		FILL_FAST_BUFFER(unsigned short);
	}
	else {
		FILL_FAST_BUFFER(unsigned int);
	}

	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);

	if (totquad == fully_visible_totquad) {
		buffers->index_buf = gpu_get_grid_buffer(gridsize, &buffers->index_type, &buffers->tot_quad);
		buffers->has_hidden = 0;
	}
	else {
		buffers->tot_quad = totquad;

		if (totgrid * gridsize * gridsize < USHRT_MAX) {
			buffers->index_type = GL_UNSIGNED_SHORT;
			FILL_QUAD_BUFFER(unsigned short, totquad, buffers->index_buf);
		}
		else {
			buffers->index_type = GL_UNSIGNED_INT;
			FILL_QUAD_BUFFER(unsigned int, totquad, buffers->index_buf);
		}

		glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
		buffers->has_hidden = 1;
	}

	/* Build coord/normal VBO */
	if (buffers->index_buf)
		buffers->vert_buf = GPU_buffer_alloc(sizeof(VertexBufferFormat) * totgrid * key->grid_area, false);

	return buffers;
}

#undef FILL_QUAD_BUFFER

/* Output a BMVert into a VertexBufferFormat array
 *
 * The vertex is skipped if hidden, otherwise the output goes into
 * index '*v_index' in the 'vert_data' array and '*v_index' is
 * incremented.
 */
static void gpu_bmesh_vert_to_buffer_copy(BMVert *v,
                                          VertexBufferFormat *vert_data,
                                          int *v_index,
                                          const float fno[3],
                                          const float *fmask,
                                          const int cd_vert_mask_offset,
                                          const float diffuse_color[4])
{
	if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
		VertexBufferFormat *vd = &vert_data[*v_index];

		/* Set coord, normal, and mask */
		copy_v3_v3(vd->co, v->co);
		normal_float_to_short_v3(vd->no, fno ? fno : v->no);

		gpu_color_from_mask_copy(
		        fmask ? *fmask :
		                BM_ELEM_CD_GET_FLOAT(v, cd_vert_mask_offset),
		        diffuse_color,
		        vd->color);

		/* Assign index for use in the triangle index buffer */
		/* note: caller must set:  bm->elem_index_dirty |= BM_VERT; */
		BM_elem_index_set(v, (*v_index)); /* set_dirty! */

		(*v_index)++;
	}
}

/* Return the total number of vertices that don't have BM_ELEM_HIDDEN set */
static int gpu_bmesh_vert_visible_count(GSet *bm_unique_verts,
                                        GSet *bm_other_verts)
{
	GSetIterator gs_iter;
	int totvert = 0;

	GSET_ITER (gs_iter, bm_unique_verts) {
		BMVert *v = BLI_gsetIterator_getKey(&gs_iter);
		if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN))
			totvert++;
	}
	GSET_ITER (gs_iter, bm_other_verts) {
		BMVert *v = BLI_gsetIterator_getKey(&gs_iter);
		if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN))
			totvert++;
	}

	return totvert;
}

/* Return the total number of visible faces */
static int gpu_bmesh_face_visible_count(GSet *bm_faces)
{
	GSetIterator gh_iter;
	int totface = 0;

	GSET_ITER (gh_iter, bm_faces) {
		BMFace *f = BLI_gsetIterator_getKey(&gh_iter);

		if (!BM_elem_flag_test(f, BM_ELEM_HIDDEN))
			totface++;
	}

	return totface;
}

/* Creates a vertex buffer (coordinate, normal, color) and, if smooth
 * shading, an element index buffer. */
void GPU_update_bmesh_pbvh_buffers(GPU_PBVH_Buffers *buffers,
                                   BMesh *bm,
                                   GSet *bm_faces,
                                   GSet *bm_unique_verts,
                                   GSet *bm_other_verts,
                                   bool show_diffuse_color)
{
	VertexBufferFormat *vert_data;
	void *tri_data;
	int tottri, totvert, maxvert = 0;
	float diffuse_color[4] = {0.8f, 0.8f, 0.8f, 1.0f};

	/* TODO, make mask layer optional for bmesh buffer */
	const int cd_vert_mask_offset = CustomData_get_offset(&bm->vdata, CD_PAINT_MASK);

	buffers->show_diffuse_color = show_diffuse_color;
	buffers->use_matcaps = GPU_material_use_matcaps_get();

	/* Count visible triangles */
	tottri = gpu_bmesh_face_visible_count(bm_faces);

	if (buffers->smooth) {
		/* Count visible vertices */
		totvert = gpu_bmesh_vert_visible_count(bm_unique_verts, bm_other_verts);
	}
	else
		totvert = tottri * 3;

	if (!tottri) {
		buffers->tot_tri = 0;
		return;
	}

	if (buffers->use_matcaps)
		diffuse_color[0] = diffuse_color[1] = diffuse_color[2] = 1.0;
	else if (show_diffuse_color) {
		/* due to dynamic nature of dyntopo, only get first material */
		GSetIterator gs_iter;
		BMFace *f;
		BLI_gsetIterator_init(&gs_iter, bm_faces);
		f = BLI_gsetIterator_getKey(&gs_iter);
		GPU_material_diffuse_get(f->mat_nr + 1, diffuse_color);
	}

	copy_v4_v4(buffers->diffuse_color, diffuse_color);

	/* Initialize vertex buffer */
	if (buffers->vert_buf)
		GPU_buffer_free(buffers->vert_buf);
	buffers->vert_buf = GPU_buffer_alloc(sizeof(VertexBufferFormat) * totvert, false);

	/* Fill vertex buffer */
	vert_data = GPU_buffer_lock(buffers->vert_buf, GPU_BINDING_ARRAY);
	if (vert_data) {
		int v_index = 0;

		if (buffers->smooth) {
			GSetIterator gs_iter;

			/* Vertices get an index assigned for use in the triangle
			 * index buffer */
			bm->elem_index_dirty |= BM_VERT;

			GSET_ITER (gs_iter, bm_unique_verts) {
				gpu_bmesh_vert_to_buffer_copy(BLI_gsetIterator_getKey(&gs_iter),
				                              vert_data, &v_index, NULL, NULL,
				                              cd_vert_mask_offset, diffuse_color);
			}

			GSET_ITER (gs_iter, bm_other_verts) {
				gpu_bmesh_vert_to_buffer_copy(BLI_gsetIterator_getKey(&gs_iter),
				                              vert_data, &v_index, NULL, NULL,
				                              cd_vert_mask_offset, diffuse_color);
			}

			maxvert = v_index;
		}
		else {
			GSetIterator gs_iter;

			GSET_ITER (gs_iter, bm_faces) {
				BMFace *f = BLI_gsetIterator_getKey(&gs_iter);

				BLI_assert(f->len == 3);

				if (!BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
					BMVert *v[3];
					float fmask = 0;
					int i;

#if 0
					BM_iter_as_array(bm, BM_VERTS_OF_FACE, f, (void**)v, 3);
#endif
					BM_face_as_array_vert_tri(f, v);

					/* Average mask value */
					for (i = 0; i < 3; i++) {
						fmask += BM_ELEM_CD_GET_FLOAT(v[i], cd_vert_mask_offset);
					}
					fmask /= 3.0f;
					
					for (i = 0; i < 3; i++) {
						gpu_bmesh_vert_to_buffer_copy(v[i], vert_data,
						                              &v_index, f->no, &fmask,
						                              cd_vert_mask_offset, diffuse_color);
					}
				}
			}

			buffers->tot_tri = tottri;
		}

		GPU_buffer_unlock(buffers->vert_buf, GPU_BINDING_ARRAY);

		/* gpu_bmesh_vert_to_buffer_copy sets dirty index values */
		bm->elem_index_dirty |= BM_VERT;
	}
	else {
		/* Memory map failed */
		GPU_buffer_free(buffers->vert_buf);
		buffers->vert_buf = NULL;
		return;
	}

	if (buffers->smooth) {
		const int use_short = (maxvert < USHRT_MAX);

		/* Initialize triangle index buffer */
		if (buffers->index_buf)
			GPU_buffer_free(buffers->index_buf);
		buffers->index_buf = GPU_buffer_alloc((use_short ?
		                                      sizeof(unsigned short) :
		                                      sizeof(unsigned int)) * 3 * tottri, false);

		/* Fill triangle index buffer */
		tri_data = GPU_buffer_lock(buffers->index_buf, GPU_BINDING_INDEX);
		if (tri_data) {
			GSetIterator gs_iter;

			GSET_ITER (gs_iter, bm_faces) {
				BMFace *f = BLI_gsetIterator_getKey(&gs_iter);

				if (!BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
					BMLoop *l_iter;
					BMLoop *l_first;

					l_iter = l_first = BM_FACE_FIRST_LOOP(f);
					do {
						BMVert *v = l_iter->v;
						if (use_short) {
							unsigned short *elem = tri_data;
							(*elem) = BM_elem_index_get(v);
							elem++;
							tri_data = elem;
						}
						else {
							unsigned int *elem = tri_data;
							(*elem) = BM_elem_index_get(v);
							elem++;
							tri_data = elem;
						}
					} while ((l_iter = l_iter->next) != l_first);
				}
			}

			GPU_buffer_unlock(buffers->index_buf, GPU_BINDING_INDEX);

			buffers->tot_tri = tottri;
			buffers->index_type = (use_short ?
								   GL_UNSIGNED_SHORT :
								   GL_UNSIGNED_INT);
		}
		else {
			/* Memory map failed */
			GPU_buffer_free(buffers->index_buf);
			buffers->index_buf = NULL;
		}
	}
	else if (buffers->index_buf) {
		GPU_buffer_free(buffers->index_buf);
	}
}

GPU_PBVH_Buffers *GPU_build_bmesh_pbvh_buffers(int smooth_shading)
{
	GPU_PBVH_Buffers *buffers;

	buffers = MEM_callocN(sizeof(GPU_PBVH_Buffers), "GPU_Buffers");
	buffers->use_bmesh = true;
	buffers->smooth = smooth_shading;
	buffers->show_diffuse_color = false;
	buffers->use_matcaps = false;

	return buffers;
}

void GPU_draw_pbvh_buffers(GPU_PBVH_Buffers *buffers, DMSetMaterial setMaterial,
                           bool wireframe, bool fast)
{
	bool do_fast = fast && buffers->index_buf_fast;
	/* sets material from the first face, to solve properly face would need to
	 * be sorted in buckets by materials */
	if (setMaterial) {
		if (buffers->face_indices_len) {
			const MLoopTri *lt = &buffers->looptri[buffers->face_indices[0]];
			const MPoly *mp = &buffers->mpoly[lt->poly];
			if (!setMaterial(mp->mat_nr + 1, NULL))
				return;
		}
		else if (buffers->totgrid) {
			const DMFlagMat *f = &buffers->grid_flag_mats[buffers->grid_indices[0]];
			if (!setMaterial(f->mat_nr + 1, NULL))
				return;
		}
		else {
			if (!setMaterial(1, NULL))
				return;
		}
	}

	glShadeModel((buffers->smooth || buffers->face_indices_len) ? GL_SMOOTH : GL_FLAT);

	if (buffers->vert_buf) {
		char *base = NULL;
		char *index_base = NULL;
		glEnableClientState(GL_VERTEX_ARRAY);
		if (!wireframe) {
			glEnableClientState(GL_NORMAL_ARRAY);
			gpu_colors_enable(VBO_ENABLED);
		}

		GPU_buffer_bind(buffers->vert_buf, GPU_BINDING_ARRAY);

		if (!buffers->vert_buf->use_vbo)
			base = (char *)buffers->vert_buf->pointer;

		if (do_fast) {
			GPU_buffer_bind(buffers->index_buf_fast, GPU_BINDING_INDEX);
			if (!buffers->index_buf_fast->use_vbo)
				index_base = buffers->index_buf_fast->pointer;
		}
		else if (buffers->index_buf) {
			GPU_buffer_bind(buffers->index_buf, GPU_BINDING_INDEX);
			if (!buffers->index_buf->use_vbo)
				index_base = buffers->index_buf->pointer;
		}

		if (wireframe)
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

		if (buffers->tot_quad) {
			const char *offset = base;
			int i, last = buffers->has_hidden ? 1 : buffers->totgrid;
			for (i = 0; i < last; i++) {
				glVertexPointer(3, GL_FLOAT, sizeof(VertexBufferFormat),
				                offset + offsetof(VertexBufferFormat, co));
				glNormalPointer(GL_SHORT, sizeof(VertexBufferFormat),
				                offset + offsetof(VertexBufferFormat, no));
				glColorPointer(3, GL_UNSIGNED_BYTE, sizeof(VertexBufferFormat),
				               offset + offsetof(VertexBufferFormat, color));
				
				if (do_fast)
					glDrawElements(GL_TRIANGLES, buffers->totgrid * 6, buffers->index_type, index_base);
				else
					glDrawElements(GL_TRIANGLES, buffers->tot_quad * 6, buffers->index_type, index_base);

				offset += buffers->gridkey.grid_area * sizeof(VertexBufferFormat);
			}
		}
		else if (buffers->tot_tri) {
			int totelem = buffers->tot_tri * 3;

			glVertexPointer(3, GL_FLOAT, sizeof(VertexBufferFormat),
			                (void *)(base + offsetof(VertexBufferFormat, co)));
			glNormalPointer(GL_SHORT, sizeof(VertexBufferFormat),
			                (void *)(base + offsetof(VertexBufferFormat, no)));
			glColorPointer(3, GL_UNSIGNED_BYTE, sizeof(VertexBufferFormat),
			               (void *)(base + offsetof(VertexBufferFormat, color)));

			if (buffers->index_buf)
				glDrawElements(GL_TRIANGLES, totelem, buffers->index_type, index_base);
			else
				glDrawArrays(GL_TRIANGLES, 0, totelem);
		}

		if (wireframe)
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
		if (buffers->index_buf || do_fast)
			glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);

		glDisableClientState(GL_VERTEX_ARRAY);
		if (!wireframe) {
			glDisableClientState(GL_NORMAL_ARRAY);
			gpu_colors_disable(VBO_ENABLED);
		}
	}
}

bool GPU_pbvh_buffers_diffuse_changed(GPU_PBVH_Buffers *buffers, GSet *bm_faces, bool show_diffuse_color)
{
	float diffuse_color[4];
	bool use_matcaps = GPU_material_use_matcaps_get();

	if (buffers->show_diffuse_color != show_diffuse_color)
		return true;

	if (buffers->use_matcaps != use_matcaps)
		return true;

	if ((buffers->show_diffuse_color == false) || use_matcaps)
		return false;

	if (buffers->looptri) {
		const MLoopTri *lt = &buffers->looptri[buffers->face_indices[0]];
		const MPoly *mp = &buffers->mpoly[lt->poly];

		GPU_material_diffuse_get(mp->mat_nr + 1, diffuse_color);
	}
	else if (buffers->use_bmesh) {
		/* due to dynamic nature of dyntopo, only get first material */
		if (BLI_gset_size(bm_faces) > 0) {
			GSetIterator gs_iter;
			BMFace *f;

			BLI_gsetIterator_init(&gs_iter, bm_faces);
			f = BLI_gsetIterator_getKey(&gs_iter);
			GPU_material_diffuse_get(f->mat_nr + 1, diffuse_color);
		}
		else {
			return false;
		}
	}
	else {
		const DMFlagMat *flags = &buffers->grid_flag_mats[buffers->grid_indices[0]];

		GPU_material_diffuse_get(flags->mat_nr + 1, diffuse_color);
	}

	return !equals_v3v3(diffuse_color, buffers->diffuse_color);
}

void GPU_free_pbvh_buffers(GPU_PBVH_Buffers *buffers)
{
	if (buffers) {
		if (buffers->vert_buf)
			GPU_buffer_free(buffers->vert_buf);
		if (buffers->index_buf && (buffers->tot_tri || buffers->has_hidden))
			GPU_buffer_free(buffers->index_buf);
		if (buffers->index_buf_fast)
			GPU_buffer_free(buffers->index_buf_fast);

		MEM_freeN(buffers);
	}
}


/* debug function, draws the pbvh BB */
void GPU_draw_pbvh_BB(float min[3], float max[3], bool leaf)
{
	const float quads[4][4][3] = {
		{
			{min[0], min[1], min[2]},
			{max[0], min[1], min[2]},
			{max[0], min[1], max[2]},
			{min[0], min[1], max[2]}
		},

		{
			{min[0], min[1], min[2]},
			{min[0], max[1], min[2]},
			{min[0], max[1], max[2]},
			{min[0], min[1], max[2]}
		},

		{
			{max[0], max[1], min[2]},
			{max[0], min[1], min[2]},
			{max[0], min[1], max[2]},
			{max[0], max[1], max[2]}
		},

		{
			{max[0], max[1], min[2]},
			{min[0], max[1], min[2]},
			{min[0], max[1], max[2]},
			{max[0], max[1], max[2]}
		},
	};

	if (leaf)
		glColor4f(0.0, 1.0, 0.0, 0.5);
	else
		glColor4f(1.0, 0.0, 0.0, 0.5);

	glVertexPointer(3, GL_FLOAT, 0, &quads[0][0][0]);
	glDrawArrays(GL_QUADS, 0, 16);
}

void GPU_init_draw_pbvh_BB(void)
{
	glPushAttrib(GL_ENABLE_BIT);
	glDisable(GL_CULL_FACE);
	glEnableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	glDisable(GL_LIGHTING);
	glDisable(GL_COLOR_MATERIAL);
	glEnable(GL_BLEND);
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
}

void GPU_end_draw_pbvh_BB(void)
{
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glPopAttrib();
}
