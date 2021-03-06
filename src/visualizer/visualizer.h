#ifndef SIMIT_VISUALIZER_VISUALIZER_H
#define SIMIT_VISUALIZER_VISUALIZER_H

#include <functional>
#include <pthread.h>

#include "graph.h"

namespace simit {

void initDrawing(int argc, char** argv);
void initDrawing();

// Non-blocking functions (window is only displayed as long as
// program continues executing)
void drawPoints(const Set &points, FieldRef<double,3> coordField,
                float r, float g, float b, float a);
void drawEdges(Set &edges, FieldRef<double,3> coordField,
               float r, float g, float b, float a);
void drawFaces(Set &faces, FieldRef<double,3> coordField,
               float r, float g, float b, float a);

// Blocking functions with callbacks for animation
void drawPointsBlocking(const Set &points, FieldRef<double,3> coordField,
                        float r, float g, float b, float a,
                        std::function<void()> animate);
void drawEdgesBlocking(Set &edges, FieldRef<double,3> coordField,
                       float r, float g, float b, float a,
                       std::function<void()> animate);
void drawFacesBlocking(Set &faces, FieldRef<double,3> coordField,
                       float r, float g, float b, float a,
                       std::function<void()> animate);

// Blocking function without callbacks for animation
void drawPointsBlocking(const Set &points, FieldRef<double,3> coordField,
                        float r, float g, float b, float a);
void drawEdgesBlocking(Set &edges, FieldRef<double,3> coordField,
                       float r, float g, float b, float a);
void drawFacesBlocking(Set &faces, FieldRef<double,3> coordField,
                       float r, float g, float b, float a);

} // namespace simit

#endif // SIMIT_VISUALIZER_VISUALIZER_H
