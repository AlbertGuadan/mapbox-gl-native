#include <mbgl/renderer/fill_bucket.hpp>
#include <mbgl/style/layers/fill_layer.hpp>
#include <mbgl/renderer/painter.hpp>
#include <mbgl/shader/plain_shader.hpp>
#include <mbgl/shader/pattern_shader.hpp>
#include <mbgl/shader/outline_shader.hpp>
#include <mbgl/shader/outlinepattern_shader.hpp>
#include <mbgl/gl/gl.hpp>
#include <mbgl/platform/log.hpp>

#include <mapbox/earcut.hpp>

#include <cassert>

namespace mapbox {
namespace util {
template <> struct nth<0, mbgl::GeometryCoordinate> {
    static int64_t get(const mbgl::GeometryCoordinate& t) { return t.x; };
};

template <> struct nth<1, mbgl::GeometryCoordinate> {
    static int64_t get(const mbgl::GeometryCoordinate& t) { return t.y; };
};
} // namespace util
} // namespace mapbox

namespace mbgl {

using namespace style;

struct GeometryTooLongException : std::exception {};

FillBucket::FillBucket() {
}

FillBucket::~FillBucket() = default;

void FillBucket::addGeometry(const GeometryCollection& geometry) {
    for (auto& polygon : classifyRings(geometry)) {
        // Optimize polygons with many interior rings for earcut tesselation.
        limitHoles(polygon, 500);

        std::size_t totalVertices = 0;

        for (const auto& ring : polygon) {
            totalVertices += ring.size();
            if (totalVertices > 65535)
                throw GeometryTooLongException();
        }

        for (const auto& ring : polygon) {
            std::size_t nVertices = ring.size();

            if (nVertices == 0)
                continue;

            if (lineGroups.empty() || lineGroups.back()->vertex_length + nVertices > 65535)
                lineGroups.emplace_back(std::make_unique<LineGroup>());

            LineGroup& lineGroup = *lineGroups.back();
            uint16_t lineIndex = lineGroup.vertex_length;

            vertices.emplace_back(ring[0].x, ring[0].y);
            lines.emplace_back(static_cast<uint16_t>(lineIndex + nVertices - 1),
                               static_cast<uint16_t>(lineIndex));

            for (uint32_t i = 1; i < nVertices; i++) {
                vertices.emplace_back(ring[i].x, ring[i].y);
                lines.emplace_back(static_cast<uint16_t>(lineIndex + i - 1),
                                   static_cast<uint16_t>(lineIndex + i));
            }

            lineGroup.vertex_length += nVertices;
            lineGroup.elements_length += nVertices;
        }

        std::vector<uint32_t> indices = mapbox::earcut(polygon);

        std::size_t nIndicies = indices.size();
        assert(nIndicies % 3 == 0);

        if (triangleGroups.empty() || triangleGroups.back()->vertex_length + totalVertices > 65535) {
            triangleGroups.emplace_back(std::make_unique<TriangleGroup>());
        }

        TriangleGroup& triangleGroup = *triangleGroups.back();
        uint16_t triangleIndex = triangleGroup.vertex_length;

        for (uint32_t i = 0; i < nIndicies; i += 3) {
            triangles.emplace_back(static_cast<uint16_t>(triangleIndex + indices[i]),
                                   static_cast<uint16_t>(triangleIndex + indices[i + 1]),
                                   static_cast<uint16_t>(triangleIndex + indices[i + 2]));
        }

        triangleGroup.vertex_length += totalVertices;
        triangleGroup.elements_length += nIndicies / 3;
    }
}

void FillBucket::upload(gl::Context& context) {
    vertexBuffer = context.createVertexBuffer(std::move(vertices));
    lineIndexBuffer = context.createIndexBuffer(std::move(lines));
    triangleIndexBuffer = context.createIndexBuffer(std::move(triangles));

    // From now on, we're going to render during the opaque and translucent pass.
    uploaded = true;
}

void FillBucket::render(Painter& painter,
                        PaintParameters& parameters,
                        const Layer& layer,
                        const RenderTile& tile) {
    painter.renderFill(parameters, *this, *layer.as<FillLayer>(), tile);
}

bool FillBucket::hasData() const {
    return !triangleGroups.empty() || !lineGroups.empty();
}

bool FillBucket::needsClipping() const {
    return true;
}

void FillBucket::drawElements(PlainShader& shader,
                              gl::Context& context,
                              PaintMode paintMode) {
    GLbyte* vertex_index = BUFFER_OFFSET(0);
    GLbyte* elements_index = BUFFER_OFFSET(0);
    for (auto& group : triangleGroups) {
        assert(group);
        group->array[paintMode == PaintMode::Overdraw ? 1 : 0].bind(
            shader, *vertexBuffer, *triangleIndexBuffer, vertex_index, context);
        MBGL_CHECK_ERROR(glDrawElements(GL_TRIANGLES, group->elements_length * 3, GL_UNSIGNED_SHORT,
                                        elements_index));
        vertex_index += group->vertex_length * vertexBuffer->vertexSize;
        elements_index += group->elements_length * triangleIndexBuffer->primitiveSize;
    }
}

void FillBucket::drawElements(PatternShader& shader,
                              gl::Context& context,
                              PaintMode paintMode) {
    GLbyte* vertex_index = BUFFER_OFFSET(0);
    GLbyte* elements_index = BUFFER_OFFSET(0);
    for (auto& group : triangleGroups) {
        assert(group);
        group->array[paintMode == PaintMode::Overdraw ? 3 : 2].bind(
            shader, *vertexBuffer, *triangleIndexBuffer, vertex_index, context);
        MBGL_CHECK_ERROR(glDrawElements(GL_TRIANGLES, group->elements_length * 3, GL_UNSIGNED_SHORT,
                                        elements_index));
        vertex_index += group->vertex_length * vertexBuffer->vertexSize;
        elements_index += group->elements_length * triangleIndexBuffer->primitiveSize;
    }
}

void FillBucket::drawVertices(OutlineShader& shader,
                              gl::Context& context,
                              PaintMode paintMode) {
    GLbyte* vertex_index = BUFFER_OFFSET(0);
    GLbyte* elements_index = BUFFER_OFFSET(0);
    for (auto& group : lineGroups) {
        assert(group);
        group->array[paintMode == PaintMode::Overdraw ? 1 : 0].bind(
            shader, *vertexBuffer, *lineIndexBuffer, vertex_index, context);
        MBGL_CHECK_ERROR(glDrawElements(GL_LINES, group->elements_length * 2, GL_UNSIGNED_SHORT,
                                        elements_index));
        vertex_index += group->vertex_length * vertexBuffer->vertexSize;
        elements_index += group->elements_length * lineIndexBuffer->primitiveSize;
    }
}

void FillBucket::drawVertices(OutlinePatternShader& shader,
                              gl::Context& context,
                              PaintMode paintMode) {
    GLbyte* vertex_index = BUFFER_OFFSET(0);
    GLbyte* elements_index = BUFFER_OFFSET(0);
    for (auto& group : lineGroups) {
        assert(group);
        group->array[paintMode == PaintMode::Overdraw ? 3 : 2].bind(
            shader, *vertexBuffer, *lineIndexBuffer, vertex_index, context);
        MBGL_CHECK_ERROR(glDrawElements(GL_LINES, group->elements_length * 2, GL_UNSIGNED_SHORT,
                                        elements_index));
        vertex_index += group->vertex_length * vertexBuffer->vertexSize;
        elements_index += group->elements_length * lineIndexBuffer->primitiveSize;
    }
}

} // namespace mbgl
