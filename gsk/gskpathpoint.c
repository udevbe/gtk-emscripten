#include "config.h"

#include "gskpathpointprivate.h"

#include "gskcontourprivate.h"

#include "gdk/gdkprivate.h"


/**
 * GskPathPoint:
 *
 * `GskPathPoint` is an opaque, immutable type representing a point on a path.
 *
 * It can be queried for properties of the path at that point, such as its
 * tangent or its curvature.
 *
 * To obtain a `GskPathPoint`, use [method@Gsk.PathMeasure.get_path_point]
 * or [method@Gsk.PathMeasure.get_closest_point].
 */

G_DEFINE_BOXED_TYPE (GskPathPoint, gsk_path_point,
                     gsk_path_point_ref,
                     gsk_path_point_unref)


GskPathPoint *
gsk_path_point_new (GskPath *path)
{
  GskPathPoint *self;

  self = g_new0 (GskPathPoint, 1);

  self->ref_count = 1;

  self->path = gsk_path_ref (path);

  return self;
}

GskPath *
gsk_path_point_get_path (GskPathPoint *self)
{
  return self->path;
}

const GskContour *
gsk_path_point_get_contour (GskPathPoint *self)
{
  return self->contour;
}

/**
 * gsk_path_point_ref:
 * @self: a `GskPathPoint`
 *
 * Increases the reference count of a `GskPathPoint` by one.
 *
 * Returns: the passed in `GskPathPoint`
 */
GskPathPoint *
gsk_path_point_ref (GskPathPoint *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  self->ref_count++;

  return self;
}

/**
 * gsk_path_point_unref:
 * @self: a `GskPathPoint`
 *
 * Decreases the reference count of a `GskPathPoint` by one.
 *
 * If the resulting reference count is zero, frees the path_measure.
 */
void
gsk_path_point_unref (GskPathPoint *self)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (self->ref_count > 0);

  self->ref_count--;
  if (self->ref_count > 0)
    return;

  gsk_path_unref (self->path);
  g_free (self);
}

/**
 * gsk_path_point_get_position:
 * @self: a `GskPathPoint`
 * @position: (out caller-allocates): Return location for
 *   the coordinates of the point
 *
 * Gets the position of the point.
 */
void
gsk_path_point_get_position (GskPathPoint     *self,
                             graphene_point_t *position)
{
  gsk_contour_get_position (self->contour, self, position);
}

/**
 * gsk_path_point_get_tangent:
 * @self: a `GskPathPoint`
 * @direction: the direction for which to return the tangent
 * @tangent: (out caller-allocates): Return location for
 *   the tangent at the point
 *
 * Gets the tangent of the path at the point.
 *
 * Note that certain points on a path may not have a single
 * tangent, such as sharp turns. At such points, there are
 * two tangents -- the direction of the path going into the
 * point, and the direction coming out of it.
 *
 * The @direction argument lets you choose which one to get.
 */
void
gsk_path_point_get_tangent (GskPathPoint     *self,
                            GskPathDirection  direction,
                            graphene_vec2_t  *tangent)
{
  gsk_contour_get_tangent (self->contour, self, direction, tangent);
}

/**
 * gsk_path_point_get_curvature:
 * @self: a `GskPathPoint`
 * @center: (out caller-allocates): Return location for
 *   the center of the osculating circle
 *
 * Calculates the curvature at the point @distance units into
 * the path.
 *
 * Optionally, returns the center of the osculating circle as well.
 *
 * If the curvature is infinite (at line segments), zero is returned,
 * and @center is not modified.
 *
 * Returns: The curvature of the path at the given point
 */
float
gsk_path_point_get_curvature (GskPathPoint     *self,
                              graphene_point_t *center)
{
  return gsk_contour_get_curvature (self->contour, self, center);
}