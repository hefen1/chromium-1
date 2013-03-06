// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/custom_filter_renderer.h"

#include <iostream>

#include "cc/custom_filter_mesh.h"
#include "cc/gl_renderer.h"
#include "third_party/khronos/GLES2/gl2.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebCustomFilterProgram.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebCString.h"

namespace cc {

scoped_ptr<CustomFilterRenderer> CustomFilterRenderer::create(WebKit::WebGraphicsContext3D* context)
{
    scoped_ptr<CustomFilterRenderer> renderer(make_scoped_ptr(new CustomFilterRenderer(context)));
    return renderer.Pass();
}

CustomFilterRenderer::CustomFilterRenderer(WebKit::WebGraphicsContext3D* context)
    : m_context(context)
{
}

CustomFilterRenderer::~CustomFilterRenderer()
{
}

static int inx(int row, int column)
{
    return 4 * (column - 1) + (row - 1);
}

static void identityMatrix(float matrix[16])
{
    for (int i = 0; i < 16; i++)
        matrix[i] = (i % 5 == 0) ? 1.0 : 0.0;
}

static void orthogonalProjectionMatrix(float matrix[16], float left, float right, float bottom, float top)
{
    // Start with an identity matrix.
    identityMatrix(matrix);

    float deltaX = right - left;
    float deltaY = top - bottom;
    if (!deltaX || !deltaY)
        return;
    matrix[inx(1, 1)] = 2.0 / deltaX;
    matrix[inx(4, 1)] = -(right + left) / deltaX;
    matrix[inx(2, 2)] = 2.0 / deltaY;
    matrix[inx(4, 2)] = -(top + bottom) / deltaY;

    // Use big enough near/far values, so that simple rotations of rather large objects will not
    // get clipped. 10000 should cover most of the screen resolutions.
    const float farValue = 10000;
    const float nearValue = -10000;
    matrix[inx(3, 3)] = -2.0 / (farValue - nearValue);
    matrix[inx(4, 3)] = -(farValue + nearValue) / (farValue - nearValue);
}

static WebKit::WebGLId createShader(WebKit::WebGraphicsContext3D* context, WebKit::WGC3Denum type, const WebKit::WGC3Dchar* source)
{
    WebKit::WebGLId shader = GLC(context, context->createShader(type));
    // std::cerr << "Created shader with id " << shader << "." << std::endl;
    GLC(context, context->shaderSource(shader, source));
    GLC(context, context->compileShader(shader));
    WebKit::WGC3Dint compileStatus = 0;
    GLC(context, context->getShaderiv(shader, GL_COMPILE_STATUS, &compileStatus));
    if (!compileStatus) {
        std::cerr << "Failed to compile shader." << std::endl;
        GLC(context, context->deleteShader(shader));
        return 0;
    } else {
        // std::cerr << "Compiled shader." << std::endl;
        return shader;
    }
}

static WebKit::WebGLId createProgram(WebKit::WebGraphicsContext3D* context, WebKit::WebGLId vertexShader, WebKit::WebGLId fragmentShader)
{
    WebKit::WebGLId program = GLC(context, context->createProgram());
    GLC(context, context->attachShader(program, vertexShader));
    GLC(context, context->attachShader(program, fragmentShader));
    GLC(context, context->linkProgram(program));
    WebKit::WGC3Dint linkStatus = 0;
    GLC(context, context->getProgramiv(program, GL_LINK_STATUS, &linkStatus));
    if (!linkStatus) {
        std::cerr << "Failed to link program." << std::endl;
        GLC(context, context->deleteProgram(program));
        return 0;
    } else {
        // std::cerr << "Linked program." << std::endl;
        return program;
    }    
}

void CustomFilterRenderer::render(const WebKit::WebFilterOperation& op, WebKit::WebGLId sourceTextureId, WebKit::WGC3Dsizei width, WebKit::WGC3Dsizei height, WebKit::WebGLId destinationTextureId)
{
    std::cerr << "Applying custom filter with destination texture id " << destinationTextureId << " and source texture id " << sourceTextureId << "." << std::endl;

    // Set up m_context.
    if (!m_context->makeContextCurrent()) {
        std::cerr << "Failed to make custom filters m_context current." << std::endl;
        return;
    }
    // std::cerr << "Made custom filters m_context current." << std::endl;
    GLC(m_context, m_context->enable(GL_DEPTH_TEST));

    // Create frame buffer.
    WebKit::WebGLId frameBuffer = GLC(m_context, m_context->createFramebuffer());
    GLC(m_context, m_context->bindFramebuffer(GL_FRAMEBUFFER, frameBuffer));
    // std::cerr << "Created frame buffer." << std::endl;

    // Attach texture to frame buffer.
    GLC(m_context, m_context->framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destinationTextureId, 0));
    // std::cerr << "Bound texture with id " << destinationTextureId << " to frame buffer." << std::endl;

    // Set up depth buffer.
    WebKit::WebGLId depthBuffer = GLC(m_context, m_context->createRenderbuffer());
    GLC(m_context, m_context->bindRenderbuffer(GL_RENDERBUFFER, depthBuffer));
    GLC(m_context, m_context->renderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height));
    // std::cerr << "Set up depth buffer." << std::endl;

    // Attach depth buffer to frame buffer.
    GLC(m_context, m_context->framebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthBuffer));
    // std::cerr << "Attached depth buffer." << std::endl;

    // Set up viewport.
    GLC(m_context, m_context->viewport(0, 0, width, height));
    // std::cerr << "Set up viewport." << std::endl;

    // Clear render buffers.
    GLC(m_context, m_context->clearColor(1.0, 0.0, 0.0, 1.0));
    GLC(m_context, m_context->clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    // std::cerr << "Cleared render buffers." << std::endl;

    // Set up vertex shader.
    // const WebKit::WGC3Dchar* vertexShaderSource = "precision mediump float; attribute vec4 a_position; attribute vec2 css_a_texCoord; varying vec2 css_v_texCoord; void main() { css_v_texCoord = css_a_texCoord; gl_Position = a_position; }";
    const WebKit::WebCString vertexShaderString = op.customFilterProgram()->vertexShader().utf8();
    WebKit::WebGLId vertexShader = createShader(m_context, GL_VERTEX_SHADER, vertexShaderString.data());
    if (!vertexShader)
        return;

    // Set up fragment shader.
    // const WebKit::WGC3Dchar* fragmentShaderSource = "precision mediump float; uniform sampler2D css_u_texture; varying vec2 css_v_texCoord; void main() { vec4 sourceColor = texture2D(css_u_texture, css_v_texCoord); gl_FragColor = vec4(sourceColor.rgb, 1.0); }";
    const WebKit::WebCString fragmentShaderString = op.customFilterProgram()->fragmentShader().utf8();
    WebKit::WebGLId fragmentShader = createShader(m_context, GL_FRAGMENT_SHADER, fragmentShaderString.data());
    if (!fragmentShader)
        return;

    // Set up program.
    WebKit::WebGLId program = createProgram(m_context, vertexShader, fragmentShader);
    if (!program)
        return;
    GLC(m_context, m_context->useProgram(program));

    // Set up vertex buffer.
    WebKit::WebGLId vertexBuffer = GLC(m_context, m_context->createBuffer());
    // std::cerr << "Created vertex buffer." << std::endl;
    GLC(m_context, m_context->bindBuffer(GL_ARRAY_BUFFER, vertexBuffer));
    // const int numVertices = 6;
    // const int numComponentsPerVertexPosition = 4;
    // const int numComponentsPerVertexTexCoord = 2;
    // const int numComponentsPerVertex = numComponentsPerVertexPosition + numComponentsPerVertexTexCoord;
    // const int numComponents = numVertices * numComponentsPerVertex;
    // const int numBytesPerComponent = sizeof(float);
    // const int numBytesPerVertex = numComponentsPerVertex * numBytesPerComponent;
    // const int numBytes = numComponents * numBytesPerComponent;
    // const float f0 = -1.0;
    // const float f1 = 1.0;
    // const float t0 = 0.0;
    // const float t1 = 1.0;
    // const float vertices[numComponents] = {
    //     f0, f0, f0, f1, t0, t0,
    //     f1, f0, f0, f1, t1, t0,
    //     f1, f1, f0, f1, t1, t1,
    //     f0, f0, f0, f1, t0, t0,
    //     f1, f1, f0, f1, t1, t1,
    //     f0, f1, f0, f1, t0, t1
    // };
    // GLC(m_context, m_context->bufferData(GL_ARRAY_BUFFER, numBytes, vertices, GL_STATIC_DRAW));
    // std::cerr << "Uploaded vertex data." << std::endl;
    gfx::RectF meshBox(0.0, 0.0, 1.0, 1.0);
    CustomFilterMesh mesh(1, 1, meshBox, MeshTypeAttached);
    m_context->bufferData(GL_ARRAY_BUFFER, mesh.vertices().size() * sizeof(float), mesh.vertices().data(), GL_STATIC_DRAW);

    // Set up index buffer.
    WebKit::WebGLId indexBuffer = GLC(m_context, m_context->createBuffer());
    m_context->bindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
    m_context->bufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices().size() * sizeof(uint16_t), mesh.indices().data(), GL_STATIC_DRAW);

    // Bind attribute pointing to vertex positions.
    WebKit::WGC3Dint positionAttributeLocation = GLC(m_context, m_context->getAttribLocation(program, "a_position"));
    // std::cerr << "Found position attribute at location " << positionAttributeLocation << "." << std::endl;
    // HARDCODED_MESH: GLC(m_context, m_context->vertexAttribPointer(positionAttributeLocation, numComponentsPerVertexPosition, GL_FLOAT, false, numBytesPerVertex, 0));
    GLC(m_context, m_context->vertexAttribPointer(positionAttributeLocation, PositionAttribSize, GL_FLOAT, false, mesh.floatsPerVertex() * sizeof(float), PositionAttribOffset));
    GLC(m_context, m_context->enableVertexAttribArray(positionAttributeLocation));
    // std::cerr << "Bound position attribute." << std::endl;

    // Bind attribute pointing to texture coordinates.
    WebKit::WGC3Dint texCoordLocation = GLC(m_context, m_context->getAttribLocation(program, "a_texCoord"));
    // std::cerr << "Found tex coord attribute at location " << texCoordLocation << "." << std::endl;
    // HARDCODED_MESH: GLC(m_context, m_context->vertexAttribPointer(texCoordLocation, numComponentsPerVertexTexCoord, GL_FLOAT, false, numBytesPerVertex, numComponentsPerVertexPosition * numBytesPerComponent));
    GLC(m_context, m_context->vertexAttribPointer(texCoordLocation, TexAttribSize, GL_FLOAT, false, mesh.floatsPerVertex() * sizeof(float), TexAttribOffset));
    GLC(m_context, m_context->enableVertexAttribArray(texCoordLocation));
    // std::cerr << "Bound tex coord attribute." << std::endl;

    // Bind source texture.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTextureId);
    // std::cerr << "Bound source texture with id " << sourceTextureId << "." << std::endl;

    // Bind uniform pointing to source texture.
    WebKit::WGC3Dint sourceTextureUniformLocation = GLC(m_context, m_context->getUniformLocation(program, "css_u_texture"));
    glUniform1i(sourceTextureUniformLocation, 0);
    // std::cerr << "Bound source texture uniform to texture unit 0." << std::endl;

    // Bind projection matrix uniform.
    WebKit::WGC3Dint projectionMatrixLocation = GLC(m_context, m_context->getUniformLocation(program, "u_projectionMatrix"));
    float projectionMatrix[16];
    identityMatrix(projectionMatrix);
    // TODO(mvujovic): Enable the real projection matrix.
    orthogonalProjectionMatrix(projectionMatrix, -0.5, 0.5, -0.5, 0.5);
    GLC(m_context, m_context->uniformMatrix4fv(projectionMatrixLocation, 1, false, projectionMatrix));

    // Draw!
    // GLC(m_context, m_context->drawArrays(GL_TRIANGLES, 0, numVertices));
    // std::cerr << "Draw arrays!" << std::endl;
    GLC(m_context, m_context->drawElements(GL_TRIANGLES, mesh.indicesCount(), GL_UNSIGNED_SHORT, 0));
    std::cerr << "Draw elements!" << std::endl;

    // Flush.
    GLC(m_context, m_context->flush());
    std::cerr << "Flush custom filter m_context." << std::endl;
}

}  // namespace cc