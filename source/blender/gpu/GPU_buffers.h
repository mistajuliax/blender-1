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

/** \file GPU_buffers.h
 *  \ingroup gpu
 */

#ifndef __GPU_BUFFERS_H__
#define __GPU_BUFFERS_H__

#ifdef DEBUG
/*  #define DEBUG_VBO(X) printf(X)*/
#  define DEBUG_VBO(X)
#else
#  define DEBUG_VBO(X)
#endif

struct BMesh;
struct CCGElem;
struct CCGKey;
struct DMFlagMat;
struct DerivedMesh;
struct GSet;
struct GPUVertPointLink;
struct PBVH;
struct MVert;

typedef struct GPUBuffer {
	int size;        /* in bytes */
	void *pointer;   /* used with vertex arrays */
	unsigned int id; /* used with vertex buffer objects */
	bool use_vbo;    /* true for VBOs, false for vertex arrays */
} GPUBuffer;

typedef struct GPUBufferMaterial {
	/* range of points used for this material */
	int start;
	int totelements;
	int totloops;
	int *polys; /* array of polygons for this material */
	int totpolys; /* total polygons in polys */
	int counter; /* general purpose counter, initialize first! */

	/* original material index */
	short mat_nr;
} GPUBufferMaterial;

/* meshes are split up by material since changing materials requires
 * GL state changes that can't occur in the middle of drawing an
 * array.
 *
 * some simplifying assumptions are made:
 * - all quads are treated as two triangles.
 * - no vertex sharing is used; each triangle gets its own copy of the
 *   vertices it uses (this makes it easy to deal with a vertex used
 *   by faces with different properties, such as smooth/solid shading,
 *   different MCols, etc.)
 *
 * to avoid confusion between the original MVert vertices and the
 * arrays of OpenGL vertices, the latter are referred to here and in
 * the source as `points'. similarly, the OpenGL triangles generated
 * for MFaces are referred to as triangles rather than faces.
 */
typedef struct GPUDrawObject {
	GPUBuffer *points;
	GPUBuffer *normals;
	GPUBuffer *uv;
	GPUBuffer *uv_tex;
	GPUBuffer *colors;
	GPUBuffer *edges;
	GPUBuffer *uvedges;
	GPUBuffer *triangles; /* triangle index buffer */

	/* for each original vertex, the list of related points */
	struct GPUVertPointLink *vert_points;

	/* see: USE_GPU_POINT_LINK define */
#if 0
	/* storage for the vert_points lists */
	struct GPUVertPointLink *vert_points_mem;
	int vert_points_usage;
#endif
	
	int colType;
	int index_setup; /* how indices are setup, starting from start of buffer or start of material */

	GPUBufferMaterial *materials;
	int totmaterial;
	
	int tot_triangle_point;
	int tot_loose_point;
	/* different than total loops since ngons get tesselated still */
	int tot_loop_verts;
	
	/* caches of the original DerivedMesh values */
	int totvert;
	int totedge;

	int loose_edge_offset;
	int tot_loose_edge_drawn;
	int tot_edge_drawn;
} GPUDrawObject;

/* currently unused */
// #define USE_GPU_POINT_LINK

typedef struct GPUVertPointLink {
#ifdef USE_GPU_POINT_LINK
	struct GPUVertPointLink *next;
#endif
	/* -1 means uninitialized */
	int point_index;
} GPUVertPointLink;



/* used for GLSL materials */
typedef struct GPUAttrib {
	int index;
	int size;
	int type;
} GPUAttrib;

void GPU_global_buffer_pool_free(void);
void GPU_global_buffer_pool_free_unused(void);

GPUBuffer *GPU_buffer_alloc(size_t size, bool force_vertex_arrays);
void GPU_buffer_free(GPUBuffer *buffer);

void GPU_drawobject_free(struct DerivedMesh *dm);

/* free special global multires grid buffer */
void GPU_buffer_multires_free(bool force);

/* flag that controls data type to fill buffer with, a modifier will prepare. */
typedef enum {
	GPU_BUFFER_VERTEX = 0,
	GPU_BUFFER_NORMAL,
	GPU_BUFFER_COLOR,
	GPU_BUFFER_UV,
	GPU_BUFFER_UV_TEXPAINT,
	GPU_BUFFER_EDGE,
	GPU_BUFFER_UVEDGE,
	GPU_BUFFER_TRIANGLES
} GPUBufferType;

typedef enum {
	GPU_BINDING_ARRAY = 0,
	GPU_BINDING_INDEX = 1,
} GPUBindingType;

/* called before drawing */
void GPU_vertex_setup(struct DerivedMesh *dm);
void GPU_normal_setup(struct DerivedMesh *dm);
void GPU_uv_setup(struct DerivedMesh *dm);
void GPU_texpaint_uv_setup(struct DerivedMesh *dm);
/* colType is the cddata MCol type to use! */
void GPU_color_setup(struct DerivedMesh *dm, int colType);
void GPU_buffer_bind_as_color(GPUBuffer *buffer);
void GPU_edge_setup(struct DerivedMesh *dm); /* does not mix with other data */
void GPU_uvedge_setup(struct DerivedMesh *dm);

void GPU_triangle_setup(struct DerivedMesh *dm);

int GPU_attrib_element_size(GPUAttrib data[], int numdata);
void GPU_interleaved_attrib_setup(GPUBuffer *buffer, GPUAttrib data[], int numdata, int element_size);

void GPU_buffer_bind(GPUBuffer *buffer, GPUBindingType binding);

/* can't lock more than one buffer at once */
void *GPU_buffer_lock(GPUBuffer *buffer, GPUBindingType binding);
void *GPU_buffer_lock_stream(GPUBuffer *buffer, GPUBindingType binding);
void GPU_buffer_unlock(GPUBuffer *buffer, GPUBindingType binding);

/* switch color rendering on=1/off=0 */
void GPU_color_switch(int mode);

/* used for drawing edges */
void GPU_buffer_draw_elements(GPUBuffer *elements, unsigned int mode, int start, int count);

/* called after drawing */
void GPU_buffer_unbind(void);

/* only unbind interleaved data */
void GPU_interleaved_attrib_unbind(void);

/* Buffers for non-DerivedMesh drawing */
typedef struct GPU_PBVH_Buffers GPU_PBVH_Buffers;

/* build */
GPU_PBVH_Buffers *GPU_build_mesh_pbvh_buffers(
        const int (*face_vert_indices)[4],
        const struct MPoly *mpoly, const struct MLoop *mloop, const struct MLoopTri *looptri,
        const struct MVert *verts,
        const int *face_indices,
        const int  face_indices_len);

GPU_PBVH_Buffers *GPU_build_grid_pbvh_buffers(int *grid_indices, int totgrid,
                                    unsigned int **grid_hidden, int gridsize, const struct CCGKey *key);

GPU_PBVH_Buffers *GPU_build_bmesh_pbvh_buffers(int smooth_shading);

/* update */

void GPU_update_mesh_pbvh_buffers(
        GPU_PBVH_Buffers *buffers, const struct MVert *mvert,
        const int *vert_indices, int totvert, const float *vmask,
        const int (*face_vert_indices)[4], bool show_diffuse_color);

void GPU_update_bmesh_pbvh_buffers(GPU_PBVH_Buffers *buffers,
                              struct BMesh *bm,
                              struct GSet *bm_faces,
                              struct GSet *bm_unique_verts,
                              struct GSet *bm_other_verts,
                              bool show_diffuse_color);

void GPU_update_grid_pbvh_buffers(GPU_PBVH_Buffers *buffers, struct CCGElem **grids,
                             const struct DMFlagMat *grid_flag_mats,
                             int *grid_indices, int totgrid, const struct CCGKey *key,
                             bool show_diffuse_color);

/* draw */
void GPU_draw_pbvh_buffers(GPU_PBVH_Buffers *buffers, DMSetMaterial setMaterial,
                           bool wireframe, bool fast);

/* debug PBVH draw*/
void GPU_draw_pbvh_BB(float min[3], float max[3], bool leaf);
void GPU_end_draw_pbvh_BB(void);
void GPU_init_draw_pbvh_BB(void);

bool GPU_pbvh_buffers_diffuse_changed(GPU_PBVH_Buffers *buffers, struct GSet *bm_faces, bool show_diffuse_color);

void GPU_free_pbvh_buffers(GPU_PBVH_Buffers *buffers);

#endif
