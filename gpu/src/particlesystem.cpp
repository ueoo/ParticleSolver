/*
 * This class contains the main particle simulation loop.
 * Calls are made to the GPU to initialize, update, and
 * eventually terminate the simulation.
 */

#include <GL/glew.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#include <cuda_runtime.h>
#include <helper_cuda.h>
#include <thrust/host_vector.h>

#include "helper_math.h"
#include "kernel.cuh"
#include "particlesystem.h"
#include "shared_variables.cuh"
#include "util.cuh"
#include "wrappers.cuh"

/**
 * @brief ParticleSystem::ParticleSystem
 *
 *     Initializes variable and GPU memory needed
 *     for the particle simulation
 *
 * @param particleRadius
 * @param gridSize
 * @param maxParticles
 * @param minBounds
 * @param maxBounds
 * @param iterations
 */
ParticleSystem::ParticleSystem(float particleRadius, uint3 gridSize, uint maxParticles, int3 minBounds, int3 maxBounds, int iterations)
    : m_initialized(false),
      m_particleRadius(particleRadius),
      m_maxParticles(maxParticles),
      m_numParticles(0),
      //      m_dPos(0),
      m_posVbo(0),
      m_cuda_posvbo_resource(0),
      m_gridSize(gridSize),
      m_rigidIndex(0),
      m_minBounds(minBounds),
      m_maxBounds(maxBounds),
      m_solverIterations(iterations) {
    m_numGridCells = m_gridSize.x * m_gridSize.y * m_gridSize.z;

    m_gridSortBits = 18;

    // set simulation parameters
    m_params.gridSize = m_gridSize;
    m_params.numCells = m_numGridCells;
    m_params.numBodies = m_numParticles;

    m_params.particleRadius = m_particleRadius;

    m_params.worldOrigin = make_float3(0.f, 0.f, 0.f);
    float cellSize = m_params.particleRadius * 2.0f; // cell size equal to particle diameter
    m_params.cellSize = make_float3(cellSize);

    m_params.gravity = make_float3(0.0f, -9.8f, 0.0f);
    m_params.globalDamping = 1.0f;

    _init(0, maxParticles);
}

ParticleSystem::~ParticleSystem() {
    _finalize();
}

inline float frand() {
    return rand() / (float)RAND_MAX;
}

void ParticleSystem::_init(uint numParticles, uint maxParticles) {
    m_maxParticles = maxParticles;
    m_numParticles = numParticles;
    initIntegration();

    /*
     *  allocate GPU data
     */
    uint memSize = sizeof(GLfloat) * 4 * m_maxParticles;

    m_posVbo = createVBO(sizeof(GLfloat) * 4 * m_maxParticles);
    registerGLBufferObject(m_posVbo, &m_cuda_posvbo_resource);

    // grid and collisions
    allocateArray((void **)&m_dSortedPos, memSize);
    allocateArray((void **)&m_dSortedW, m_maxParticles * sizeof(float));
    allocateArray((void **)&m_dSortedPhase, m_maxParticles * sizeof(int));

    allocateArray((void **)&m_dGridParticleHash, m_maxParticles * sizeof(uint));
    allocateArray((void **)&m_dGridParticleIndex, m_maxParticles * sizeof(uint));

    allocateArray((void **)&m_dCellStart, m_numGridCells * sizeof(uint));
    allocateArray((void **)&m_dCellEnd, m_numGridCells * sizeof(uint));

    setParameters(&m_params);

    m_initialized = true;
}

void ParticleSystem::_finalize() {
    assert(m_initialized);

    freeArray(m_dSortedPos);
    freeArray(m_dSortedW);
    freeArray(m_dSortedPhase);

    freeArray(m_dGridParticleHash);
    freeArray(m_dGridParticleIndex);
    freeArray(m_dCellStart);
    freeArray(m_dCellEnd);

    unregisterGLBufferObject(m_cuda_posvbo_resource);
    glDeleteBuffers(1, (const GLuint *)&m_posVbo);

    freeIntegrationVectors();
    freeSolverVectors();
    freeSharedVectors();
}

/**
 * @brief ParticleSystem::update
 *
 *      A single step of the simulation loop.
 *      Makes calls to extern CUDA functions.
 *
 * @param deltaTime - the time (seconds) between this loop and the last one
 */
void ParticleSystem::update(float deltaTime) {
    assert(m_initialized);

    // avoid large timesteps
    deltaTime = std::min(deltaTime, .05f);

    if (m_numParticles == 0) {
        addNewStuff();
        return;
    }

    // get pointer to vbo of point positions
    // note: this should be changed eventually so the vbo can be
    // set to render things other than just points
    float *dPos = (float *)mapGLBufferObject(&m_cuda_posvbo_resource);

    // update constants
    setParameters(&m_params);

    // store current positions then guess
    // new positions based on forces
    integrateSystem(dPos,
                    deltaTime,
                    m_numParticles);

    for (uint i = 0; i < m_solverIterations; i++) {
        // calculate grid hash
        calcHash(m_dGridParticleHash,
                 m_dGridParticleIndex,
                 dPos,
                 m_numParticles);

        // sort particles based on hash
        sortParticles(m_dGridParticleHash,
                      m_dGridParticleIndex,
                      m_numParticles);

        // reorder particle arrays into sorted order and
        // find start and end of each cell
        reorderDataAndFindCellStart(
            m_dCellStart,
            m_dCellEnd,
            m_dSortedPos,
            m_dSortedW,
            m_dSortedPhase,
            m_dGridParticleHash,
            m_dGridParticleIndex,
            dPos,
            m_numParticles,
            m_numGridCells);

        // find particle neighbors and process collisions
        collide(dPos,
                m_dSortedPos,
                m_dSortedW,
                m_dSortedPhase,
                m_dGridParticleIndex,
                m_dCellStart,
                m_dCellEnd,
                m_numParticles,
                m_numGridCells);

        // find neighbors within a specified radius of fluids
        // and apply fluid constraints
        solveFluids(m_dSortedPos,
                    m_dSortedW,
                    m_dSortedPhase,
                    m_dGridParticleIndex,
                    m_dCellStart,
                    m_dCellEnd,
                    dPos,
                    m_numParticles,
                    m_numGridCells);

        // apply collision constraints for the world borders
        collideWorld(dPos,
                     m_dSortedPos,
                     m_numParticles,
                     m_minBounds,
                     m_maxBounds);

        // apply distance constraints
        solveDistanceConstraints(dPos);

        // apply point constraints
        solvePointConstraints(dPos);
    }

    // determine the current position based on distance
    // travelled during current timestep
    calcVelocity(dPos,
                 deltaTime,
                 m_numParticles);

    // unmap at end here to avoid unnecessary graphics/CUDA context switch
    unmapGLBufferObject(m_cuda_posvbo_resource);

    // add new particles to the scene
    addNewStuff();
}

void ParticleSystem::addNewStuff() {
    addParticles();
    addFluids();
}

void ParticleSystem::addParticles() {
    if (m_particlesToAdd.empty())
        return;

    uint size = m_particlesToAdd.size() / 2;
    float4 pos, vel;
    for (uint i = 0; i < size; i++) {
        pos = m_particlesToAdd.front();
        m_particlesToAdd.pop_front();
        vel = m_particlesToAdd.front();
        m_particlesToAdd.pop_front();
        addParticle(pos, make_float4(vel.x, vel.y, vel.z, 0), vel.w, 1.5f, SOLID);
    }
}

void ParticleSystem::addFluids() {
    if (m_fluidsToAdd.empty())
        return;

    uint start = m_numParticles;

    uint size = m_fluidsToAdd.size() / 2;
    float4 pos, color;
    for (uint i = 0; i < size; i++) {
        pos = m_fluidsToAdd.front();
        m_fluidsToAdd.pop_front();
        color = m_fluidsToAdd.front();
        m_fluidsToAdd.pop_front();
        addParticle(make_float4(make_float3(pos), 1), make_float4(0, -1, 0, 0), pos.w, color.w, FLUID);
    }

    m_colorIndex.push_back(make_int2(start, m_numParticles));
    m_colors.push_back(make_float4(make_float3(color), 1.f));
}

void ParticleSystem::setParticleToAdd(float3 pos, float3 vel, float mass) {
    float jitter = m_particleRadius * 0.01f;
    pos.x += (frand() * 2.0f - 1.0f) * jitter;
    pos.y += (frand() * 2.0f - 1.0f) * jitter;
    m_particlesToAdd.push_back(make_float4(pos, 1.f));
    m_particlesToAdd.push_back(make_float4(vel, mass));

    m_colorIndex.push_back(make_int2(m_numParticles, m_numParticles + 1));
    m_colors.push_back(make_float4(colors[rand() % numColors], 1.f));
}

void ParticleSystem::addParticle(float4 pos, float4 vel, float mass, float ro, int phase) {
    if (m_numParticles == m_maxParticles)
        return;

    float *data = (float *)&pos;
    unregisterGLBufferObject(m_cuda_posvbo_resource);
    glBindBuffer(GL_ARRAY_BUFFER, m_posVbo);
    glBufferSubData(GL_ARRAY_BUFFER, m_numParticles * 4 * sizeof(float), 4 * sizeof(float), data);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    registerGLBufferObject(m_posVbo, &m_cuda_posvbo_resource);

    float *hv = (float *)&vel;
    float *hro = &ro;
    int *hphase = &phase;
    float w = 1.f / mass;
    float *hw = &w;

    appendIntegrationParticle(hv, hro, 1);
    appendPhaseAndMass(hphase, hw, 1);
    appendSolverParticle(1);
    m_numParticles++;
}

void ParticleSystem::addParticleMultiple(float *pos, float *vel, float *mass, float *ro, int *phase, int numParticles) {
    if (m_numParticles + numParticles >= m_maxParticles)
        return;

    unregisterGLBufferObject(m_cuda_posvbo_resource);
    glBindBuffer(GL_ARRAY_BUFFER, m_posVbo);
    glBufferSubData(GL_ARRAY_BUFFER, m_numParticles * 4 * sizeof(float), numParticles * 4 * sizeof(float), pos);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    registerGLBufferObject(m_posVbo, &m_cuda_posvbo_resource);

    appendIntegrationParticle(vel, ro, numParticles);
    appendPhaseAndMass(phase, mass, numParticles);
    appendSolverParticle(numParticles);
    m_numParticles += numParticles;
}

void ParticleSystem::setFluidToAdd(float3 pos, float3 color, float mass, float density) {
    m_fluidsToAdd.push_back(make_float4(pos, mass));
    m_fluidsToAdd.push_back(make_float4(color, density));
}

void ParticleSystem::addFluid(int3 ll, int3 ur, float mass, float density, float3 color) {
    int start = m_numParticles;
    float jitter = m_particleRadius * 0.01f;
    float distance = m_particleRadius * 2.5f /*1.667 * density*/;

    int3 count = make_int3((int)ceil(ur.x - ll.x) / distance, (int)ceil(ur.y - ll.y) / distance, (int)ceil(ur.z - ll.z) / distance);

    int arraySize = count.x * count.y * count.z;
    float pos[arraySize * 4];
    float vel[arraySize * 4];
    float w[arraySize];
    float ro[arraySize];
    int phase[arraySize];
    int index = 0;

#ifndef TWOD
    for (int z = 0; z < count.z; z++) {
#endif
        for (int y = 0; y < count.y; y++) {
            for (int x = 0; x < count.x; x++) {
                pos[index * 4] = ll.x + x * distance + (frand() * 2.0f - 1.0f) * jitter;
                pos[index * 4 + 1] = ll.y + y * distance + (frand() * 2.0f - 1.0f) * jitter;
#ifdef TWOD
                pos[index * 4 + 2] = ZPOS;
#else
            pos[index * 4 + 2] = ll.z + z * distance + (frand() * 2.0f - 1.0f) * jitter;
#endif
                pos[index * 4 + 3] = 1.f;

                index++;
            }
        }
#ifndef TWOD
    }
#endif
    memset(vel, 0, arraySize * 4 * sizeof(float));
    std::fill(w, w + arraySize, 1.f / mass);
    std::fill(ro, ro + arraySize, density);
    std::fill(phase, phase + arraySize, FLUID);

    addParticleMultiple(pos, vel, w, ro, phase, arraySize);

    m_colorIndex.push_back(make_int2(start, m_numParticles));
    m_colors.push_back(make_float4(color, 1.f));
}

void ParticleSystem::addParticleGrid(int3 ll, int3 ur, float mass, bool addJitter) {
    int start = m_numParticles;
    float jitter = 0.f;
    if (addJitter)
        jitter = m_particleRadius * 0.01f;
    float distance = m_particleRadius * 2.002f;

    int3 count = make_int3((int)ceil(ur.x - ll.x) / distance, (int)ceil(ur.y - ll.y) / distance, (int)ceil(ur.z - ll.z) / distance);

    int arraySize = count.x * count.y * count.z;
    float pos[arraySize * 4];
    float vel[arraySize * 4];
    float w[arraySize];
    float ro[arraySize];
    int phase[arraySize];
    int index = 0;

#ifndef TWOD
    for (int z = 0; z < count.z; z++) {
#endif
        for (int y = 0; y < count.y; y++) {
            for (int x = 0; x < count.x; x++) {
                pos[index * 4] = ll.x + x * distance + (frand() * 2.0f - 1.0f) * jitter;
                pos[index * 4 + 1] = ll.y + y * distance + (frand() * 2.0f - 1.0f) * jitter;
#ifdef TWOD
                pos[index * 4 + 2] = ZPOS;
#else
            pos[index * 4 + 2] = ll.z + z * distance + (frand() * 2.0f - 1.0f) * jitter;
#endif
                pos[index * 4 + 3] = 1.f;

                index++;
            }
        }
#ifndef TWOD
    }
#endif
    memset((void *)vel, 0, arraySize * 4 * sizeof(float));
    std::fill(w, w + arraySize, 1.f / mass);
    std::fill(ro, ro + arraySize, 1.f);
    std::fill(phase, phase + arraySize, SOLID);

    addParticleMultiple(pos, vel, w, ro, phase, arraySize);

    m_colorIndex.push_back(make_int2(start, m_numParticles));
    m_colors.push_back(make_float4(colors[rand() % numColors], 1.f));
}

void ParticleSystem::addHorizCloth(int2 ll, int2 ur, float3 spacing, float2 dist, float mass, bool holdEdges) {
    int start = m_numParticles;

    int2 count = make_int2((int)ceil(ur.x - ll.x) / spacing.x, (int)ceil(ur.y - ll.y) / spacing.z);

    // particle setup
    int arraySize = count.x * count.y;
    float pos[arraySize * 4];
    float vel[arraySize * 4];
    float w[arraySize];
    float ro[arraySize];
    int phase[arraySize];
    int index = 0;

    // constraint setup
    uint numDists = (count.x - 1) * (count.y - 1) + count.x * count.y - 1;
    uint numPoints;
    if (holdEdges)
        numPoints = 2 * (count.x + count.y);
    else
        numPoints = count.y;

    float points[numPoints * 3];
    uint indicesP[numPoints];
    int pi = 0, di = 0;

    uint indicesD[numDists * 2];
    float dists[numDists];

#ifndef TWOD
    for (int z = 0; z < count.y; z++) {
#endif
        for (int x = 0; x < count.x; x++) {
            pos[index * 4] = ll.x + x * spacing.x,
                        pos[index * 4 + 1] = spacing.y,
#ifdef TWOD
                        pos[index * 4 + 2] = ZPOS,
#else
                    pos[index * 4 + 2] = ll.y + z * spacing.z,
#endif
                        pos[index * 4 + 3] = 1.f;

            //            addParticle(pos, make_float4(0.f), mass, 1.f, RIGID + m_rigidIndex);

            uint particleIndex = start + z * count.x + x;
            if (x > 0) {
                indicesD[di * 2] = particleIndex - 1;
                indicesD[di * 2 + 1] = particleIndex;
                dists[di] = dist.x;
                di++;
            } else {
                indicesP[pi] = particleIndex;
                memcpy(points + pi * 3, pos + index * 4, 3 * sizeof(float));
                //                points[pi*3] = pos.x; points[pi*3+1] = pos.y; points[pi*3+2] = pos.z;
                pi++;
            }
            if (z > 0) {
                indicesD[di * 2] = particleIndex - count.x;
                indicesD[di * 2 + 1] = particleIndex;
                dists[di] = dist.y;
                di++;
            } else if (holdEdges) {
                indicesP[pi] = particleIndex;
                memcpy(points + pi * 3, pos + index * 4, 3 * sizeof(float));
                //                points[pi*3] = pos.x; points[pi*3+1] = pos.y; points[pi*3+2] = pos.z;
                pi++;
            }
            if (x == count.x - 1 && holdEdges) {
                indicesP[pi] = particleIndex;
                memcpy(points + pi * 3, pos + index * 4, 3 * sizeof(float));
                //                points[pi*3] = pos.x; points[pi*3+1] = pos.y; points[pi*3+2] = pos.z;
                pi++;
            }
            if (z == count.y - 1 && holdEdges) {
                indicesP[pi] = particleIndex;
                memcpy(points + pi * 3, pos + index * 4, 3 * sizeof(float));
                //                points[pi*3] = pos.x; points[pi*3+1] = pos.y; points[pi*3+2] = pos.z;
                pi++;
            }
            index++;
        }
#ifndef TWOD
    }
#endif
    memset((void *)vel, 0, arraySize * 4 * sizeof(float));
    std::fill(w, w + arraySize, 1.f / mass);
    std::fill(ro, ro + arraySize, 1.f);
    std::fill(phase, phase + arraySize, RIGID + m_rigidIndex /*CLOTH*/);

    addParticleMultiple(pos, vel, w, ro, phase, arraySize);
    addPointConstraint(indicesP, points, numPoints);
    addDistanceConstraint(indicesD, dists, numDists);

    m_colorIndex.push_back(make_int2(start, m_numParticles));
    m_colors.push_back(make_float4(colors[rand() % numColors], 1.f));
    m_rigidIndex++;
}

void ParticleSystem::addRope(float3 start, float3 spacing, float dist, int numLinks, float mass, bool constrainStart) {
    uint startI = m_numParticles;

    // particle setup
    int arraySize = numLinks + 1;
    float pos[arraySize * 4];
    float vel[arraySize * 4];
    float w[arraySize];
    float ro[arraySize];
    int phase[arraySize];

    pos[0] = start.x;
    pos[1] = start.y;
    pos[2] = start.z;
    pos[3] = 1.f;

    float4 pos4;

    uint indicesD[numLinks * 2];
    float dists[numLinks];
    int di = 0;

    int i;
    for (i = 1; i <= numLinks; i++) {
        pos4 = make_float4(start + i * spacing, 1.f);
        memcpy(pos + i * 4, &pos4, 4 * sizeof(float));
        //        pos[i*4] = pos4.x; pos[i*4+1] = pos4.y; pos[i*4+2] = pos4.z; pos[i*4+3] = pos4.w;

        indicesD[di * 2] = startI + i - 1;
        indicesD[di * 2 + 1] = startI + i;
        dists[di] = dist;
        di++;
    }
    memset((void *)vel, 0, arraySize * 4 * sizeof(float));
    std::fill(w, w + arraySize, 1.f / mass);
    std::fill(ro, ro + arraySize, 1.f);
    std::fill(phase, phase + arraySize, RIGID + m_rigidIndex);

    addParticleMultiple(pos, vel, w, ro, phase, arraySize);
    addDistanceConstraint(indicesD, dists, numLinks);

    if (constrainStart)
        addPointConstraint(&startI, (float *)&start, 1);

    m_colorIndex.push_back(make_int2(startI, m_numParticles));
    m_colors.push_back(make_float4(colors[rand() % numColors], 1));
    m_rigidIndex++;
}

void ParticleSystem::addStaticSphere(int3 ll, int3 ur, float spacing) {
    uint startI = m_numParticles;
    int3 count = make_int3((int)ceil(ur.x - ll.x) / spacing, (int)ceil(ur.y - ll.y) / spacing, (int)ceil(ur.z - ll.z) / spacing);
    float4 pos;
    float radius = (ur.x - ll.x) * .5f;
    float3 center = make_float3(ll) + make_float3(radius);

    // particle setup
    std::vector<float> posV;
    std::vector<uint> indices;
    std::vector<float> points;
    uint index = 0;

#ifndef TWOD
    for (int z = 0; z < count.z; z++) {
#endif
        for (int y = 0; y < count.y; y++) {
            for (int x = 0; x < count.x; x++) {

                pos = make_float4(ll.x + x * spacing,
                                  ll.y + y * spacing,
#ifdef TWOD
                                  ZPOS,
#else
                              ll.z + z * spacing,
#endif
                                  1.f);
                if (length(make_float3(pos) - center) < radius) {
                    posV.push_back(pos.x);
                    posV.push_back(pos.y);
                    posV.push_back(pos.z);
                    posV.push_back(pos.w);

                    indices.push_back(startI + index++);

                    points.push_back(pos.x);
                    points.push_back(pos.y);
                    points.push_back(pos.z);
                }
            }
        }
#ifndef TWOD
    }
#endif
    int arraySize = indices.size();
    float vel[arraySize * 4];
    float w[arraySize];
    float ro[arraySize];
    int phase[arraySize];

    memset((void *)vel, 0, arraySize * 4 * sizeof(float));
    std::fill(w, w + arraySize, .01f);
    std::fill(ro, ro + arraySize, 1.f);
    std::fill(phase, phase + arraySize, RIGID + m_rigidIndex);

    addParticleMultiple(posV.data(), vel, w, ro, phase, arraySize);
    addPointConstraint(indices.data(), points.data(), indices.size());

    m_colorIndex.push_back(make_int2(startI, m_numParticles));
    m_colors.push_back(make_float4(colors[rand() % numColors], 1.f));
    m_rigidIndex++;
}

void ParticleSystem::makePointConstraint(uint index, float3 point) {
    addPointConstraint(&index, (float *)&point, 1);
}

void ParticleSystem::makeDistanceConstraint(uint2 index, float distance) {
    addDistanceConstraint((uint *)&index, &distance, 1);
}

void ParticleSystem::setArray(bool isPosArray, const float *data, int start, int count) {
    assert(m_initialized);

    if (isPosArray) {
        unregisterGLBufferObject(m_cuda_posvbo_resource);
        glBindBuffer(GL_ARRAY_BUFFER, m_posVbo);
        glBufferSubData(GL_ARRAY_BUFFER, start * 4 * sizeof(float), count * 4 * sizeof(float), data);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        registerGLBufferObject(m_posVbo, &m_cuda_posvbo_resource);
    }
}

GLuint ParticleSystem::createVBO(uint size) {
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, size, 0, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return vbo;
}
