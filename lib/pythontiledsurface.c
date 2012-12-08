/* This file is part of MyPaint.
 * Copyright (C) 2008-2011 by Martin Renold <martinxyz@gmx.ch>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pythontiledsurface.h"

// caching tile memory location (optimization)
#define TILE_MEMORY_SIZE 8
typedef struct {
  int tx, ty;
  uint16_t * rgba_p;
} TileMemory;

struct _MyPaintPythonTiledSurface {
    MyPaintTiledSurface parent;

    PyObject * py_obj;
    TileMemory tileMemory[TILE_MEMORY_SIZE];
    int tileMemoryValid;
    int tileMemoryWrite;
    int atomic;
};

// Forward declare
void free_tiledsurf(MyPaintSurface *surface);

void begin_atomic(MyPaintSurface *surface)
{
    MyPaintPythonTiledSurface *self = (MyPaintPythonTiledSurface *)surface;

    mypaint_tiled_surface_begin_atomic((MyPaintTiledSurface *)self);

    if (self->atomic == 0) {
      assert(self->tileMemoryValid == 0);
    }
    self->atomic++;
}

MyPaintRectangle end_atomic(MyPaintSurface *surface)
{
    MyPaintPythonTiledSurface *self = (MyPaintPythonTiledSurface *)surface;

    MyPaintRectangle bbox = mypaint_tiled_surface_end_atomic((MyPaintTiledSurface *)self);

    assert(self->atomic > 0);
    self->atomic--;

    if (self->atomic == 0) {
        self->tileMemoryValid = 0;
        self->tileMemoryWrite = 0;

        if (bbox.width > 0) {
            PyObject* res;
            res = PyObject_CallMethod(self->py_obj, "notify_observers", "(iiii)",
                                      bbox.x, bbox.y, bbox.width, bbox.height);
            Py_DECREF(res);
        }
    }

    return bbox;
}

static void
tile_request_start(MyPaintTiledSurface *tiled_surface, MyPaintTiledSurfaceTileRequestData *request)
{
    MyPaintPythonTiledSurface *self = (MyPaintPythonTiledSurface *)tiled_surface;

    const gboolean readonly = request->readonly;
    const int tx = request->tx;
    const int ty = request->ty;

    // We assume that the memory location does not change between begin_atomic() and end_atomic().
    for (int i=0; i<self->tileMemoryValid; i++) {
      if (self->tileMemory[i].tx == tx and self->tileMemory[i].ty == ty) {
        request->buffer = self->tileMemory[i].rgba_p;
        return;
      }
    }
    if (PyErr_Occurred()) {
        request->buffer = NULL;
        return;
    }
    PyObject* rgba = PyObject_CallMethod(self->py_obj, "get_tile_memory", "(iii)", tx, ty, readonly);
    if (rgba == NULL) {
      printf("Python exception during get_tile_memory()!\n");
      request->buffer = NULL;
      return;
    }
#ifdef HEAVY_DEBUG
       assert(PyArray_NDIM(rgba) == 3);
       assert(PyArray_DIM(rgba, 0) == TILE_SIZE);
       assert(PyArray_DIM(rgba, 1) == TILE_SIZE);
       assert(PyArray_DIM(rgba, 2) == 4);
       assert(PyArray_ISCARRAY(rgba));
       assert(PyArray_TYPE(rgba) == NPY_UINT16);
#endif
    // tiledsurface.py will keep a reference in its tiledict, at least until the final end_atomic()
    Py_DECREF(rgba);
    uint16_t * rgba_p = (uint16_t*)((PyArrayObject*)rgba)->data;

    // Cache tiles to speed up small brush strokes with lots of dabs, like charcoal.
    // Not caching readonly requests; they are alternated with write requests anyway.
    if (!readonly) {
      if (self->tileMemoryValid < TILE_MEMORY_SIZE) {
        self->tileMemoryValid++;
      }
      // We always overwrite the oldest cache entry.
      // We are mainly optimizing for strokes with radius smaller than one tile.
      self->tileMemory[self->tileMemoryWrite].tx = tx;
      self->tileMemory[self->tileMemoryWrite].ty = ty;
      self->tileMemory[self->tileMemoryWrite].rgba_p = rgba_p;
      self->tileMemoryWrite = (self->tileMemoryWrite + 1) % TILE_MEMORY_SIZE;
    }

    request->buffer = rgba_p;
}

static void
tile_request_end(MyPaintTiledSurface *tiled_surface, MyPaintTiledSurfaceTileRequestData *request)
{
    // We modify tiles directly, so don't need to do anything here
}

MyPaintPythonTiledSurface *
mypaint_python_tiled_surface_new(PyObject *py_object)
{
    MyPaintPythonTiledSurface *self = (MyPaintPythonTiledSurface *)malloc(sizeof(MyPaintPythonTiledSurface));

    mypaint_tiled_surface_init(&self->parent, tile_request_start, tile_request_end);

    // MyPaintSurface vfuncs
    self->parent.parent.destroy = free_tiledsurf;
    self->parent.parent.begin_atomic = begin_atomic;
    self->parent.parent.end_atomic = end_atomic;

    self->py_obj = py_object; // no need to incref
    self->atomic = 0;
    self->tileMemoryValid = 0;
    self->tileMemoryWrite = 0;

    return self;
}

void free_tiledsurf(MyPaintSurface *surface)
{
    MyPaintPythonTiledSurface *self = (MyPaintPythonTiledSurface *)surface;
    mypaint_tiled_surface_destroy(&self->parent);
    free(self);
}
