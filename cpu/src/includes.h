#ifndef INCLUDES_H
#define INCLUDES_H

#define GLM_FORCE_RADIANS

// Standard includes
#include <iostream>
#include <stdlib.h>

// GL includes
#define GL_GLEXT_PROTOTYPES
#include <GL/glu.h>
#include <qgl.h>

// GLM includes
#include <glm.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <glm/vec2.hpp>

// Qt data includes
#include <QHash>
#include <QList>

// Generally helpful functions
inline float frand() { return (double)rand() / (double)RAND_MAX; }
inline float urand(double a, double b) { return a + (b - a) * frand(); }

using namespace std;

inline void printVec(const glm::dvec2 v) {
    cout << "(" << v.x << ", " << v.y << ")" << endl;
}

#define EPSILON .0001

#define D2R(d) (d * M_PI / 180)
#define R2D(r) (r * 180 / M_PI)

#endif // INCLUDES_H
