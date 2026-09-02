#!/usr/bin/env node
// Converts the user-authored left-hand GLB into a deliberately tiny runtime
// format understood by the Direct3D 8 proxy. No third-party npm packages are
// required: the model is static, triangle-only, and stores PNG images in GLB
// buffer views.

const fs = require('fs');
const path = require('path');

if (process.argv.length !== 4) {
  console.error('usage: node convert_lefthand_glb.js input.glb output-directory');
  process.exit(2);
}

const inputPath = process.argv[2];
const outputDirectory = process.argv[3];
const bytes = fs.readFileSync(inputPath);

if (bytes.length < 20 || bytes.toString('ascii', 0, 4) !== 'glTF' ||
    bytes.readUInt32LE(4) !== 2) {
  throw new Error('expected a glTF 2.0 binary file');
}

const jsonLength = bytes.readUInt32LE(12);
const jsonType = bytes.readUInt32LE(16);
if (jsonType !== 0x4e4f534a) {
  throw new Error('the first GLB chunk is not JSON');
}
const document = JSON.parse(bytes.toString('utf8', 20, 20 + jsonLength)
  .replace(/[\0 ]+$/g, ''));
const binHeader = 20 + ((jsonLength + 3) & ~3);
if (binHeader + 8 > bytes.length || bytes.readUInt32LE(binHeader + 4) !== 0x004e4942) {
  throw new Error('GLB BIN chunk is missing');
}
const binLength = bytes.readUInt32LE(binHeader);
const binary = bytes.subarray(binHeader + 8, binHeader + 8 + binLength);

const componentSize = new Map([[5123, 2], [5125, 4], [5126, 4]]);
const componentCount = new Map([['SCALAR', 1], ['VEC2', 2], ['VEC3', 3]]);

function readAccessor(index) {
  const accessor = document.accessors[index];
  const view = document.bufferViews[accessor.bufferView];
  const count = componentCount.get(accessor.type);
  const size = componentSize.get(accessor.componentType);
  if (!count || !size || accessor.sparse) {
    throw new Error(`unsupported accessor ${index}`);
  }
  const stride = view.byteStride || count * size;
  const start = (view.byteOffset || 0) + (accessor.byteOffset || 0);
  const result = [];
  for (let element = 0; element < accessor.count; ++element) {
    const values = [];
    for (let component = 0; component < count; ++component) {
      const offset = start + element * stride + component * size;
      if (accessor.componentType === 5126) values.push(binary.readFloatLE(offset));
      else if (accessor.componentType === 5123) values.push(binary.readUInt16LE(offset));
      else values.push(binary.readUInt32LE(offset));
    }
    result.push(values);
  }
  return result;
}

function normalize(v) {
  const length = Math.hypot(v[0], v[1], v[2]) || 1;
  return [v[0] / length, v[1] / length, v[2] / length];
}

function rotateQuaternion(v, q) {
  const [x, y, z, w] = q;
  const tx = 2 * (y * v[2] - z * v[1]);
  const ty = 2 * (z * v[0] - x * v[2]);
  const tz = 2 * (x * v[1] - y * v[0]);
  return [
    v[0] + w * tx + (y * tz - z * ty),
    v[1] + w * ty + (z * tx - x * tz),
    v[2] + w * tz + (x * ty - y * tx)
  ];
}

const meshNode = document.nodes.find(node => Number.isInteger(node.mesh));
if (!meshNode) throw new Error('no mesh node found');
const mesh = document.meshes[meshNode.mesh];
const translation = meshNode.translation || [0, 0, 0];
const rotation = meshNode.rotation || [0, 0, 0, 1];
const scale = meshNode.scale || [1, 1, 1];

const parts = mesh.primitives.map((primitive, partIndex) => {
  if ((primitive.mode ?? 4) !== 4 || primitive.attributes.POSITION === undefined ||
      primitive.attributes.TEXCOORD_0 === undefined || primitive.indices === undefined) {
    throw new Error(`primitive ${partIndex} is not an indexed textured triangle list`);
  }
  const positions = readAccessor(primitive.attributes.POSITION);
  const normals = primitive.attributes.NORMAL === undefined
    ? positions.map(() => [0, 0, 1]) : readAccessor(primitive.attributes.NORMAL);
  const texcoords = readAccessor(primitive.attributes.TEXCOORD_0);
  const indices = readAccessor(primitive.indices).map(value => value[0]);
  if (positions.length !== normals.length || positions.length !== texcoords.length ||
      positions.length > 65535 || indices.some(index => index > 65535)) {
    throw new Error(`primitive ${partIndex} has incompatible vertex data`);
  }

  const vertices = positions.map((position, index) => {
    const scaled = position.map((value, axis) => value * scale[axis]);
    const transformed = rotateQuaternion(scaled, rotation)
      .map((value, axis) => value + translation[axis]);
    const transformedNormal = normalize(rotateQuaternion(normals[index], rotation));
    // Convert glTF (+Y up, right-handed) to SH3 camera coordinates
    // (+Y down, +Z forward). Culling is disabled by the runtime because the
    // source materials are explicitly double-sided.
    return [
      transformed[0], -transformed[1], -transformed[2],
      transformedNormal[0], -transformedNormal[1], -transformedNormal[2],
      texcoords[index][0], texcoords[index][1]
    ];
  });

  // Some exported submeshes (notably fingers and the wrist watch) contain a
  // minority of triangles with winding opposite to their authored normals.
  // A single D3D cull mode then exposes their inside or removes their outside.
  // Normalize every triangle to the same geometric/normal orientation.
  let reversedTriangles = 0;
  for (let triangle = 0; triangle < indices.length; triangle += 3) {
    const i0 = indices[triangle];
    const i1 = indices[triangle + 1];
    const i2 = indices[triangle + 2];
    const p0 = vertices[i0];
    const p1 = vertices[i1];
    const p2 = vertices[i2];
    const edge1 = [p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]];
    const edge2 = [p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]];
    const faceNormal = [
      edge1[1] * edge2[2] - edge1[2] * edge2[1],
      edge1[2] * edge2[0] - edge1[0] * edge2[2],
      edge1[0] * edge2[1] - edge1[1] * edge2[0]
    ];
    const authoredNormal = [
      p0[3] + p1[3] + p2[3],
      p0[4] + p1[4] + p2[4],
      p0[5] + p1[5] + p2[5]
    ];
    const agreement = faceNormal[0] * authoredNormal[0] +
      faceNormal[1] * authoredNormal[1] +
      faceNormal[2] * authoredNormal[2];
    if (agreement < 0) {
      indices[triangle + 1] = i2;
      indices[triangle + 2] = i1;
      ++reversedTriangles;
    }
  }
  console.log(`primitive ${partIndex}: normalized ${reversedTriangles} reversed triangles`);
  return { vertices, indices, material: primitive.material ?? 0 };
});

const chunks = [];
const header = Buffer.alloc(16);
header.write('SH3LH01\0', 0, 'ascii');
header.writeUInt32LE(1, 8);
header.writeUInt32LE(parts.length, 12);
chunks.push(header);
for (const part of parts) {
  const partHeader = Buffer.alloc(12);
  partHeader.writeUInt32LE(part.vertices.length, 0);
  partHeader.writeUInt32LE(part.indices.length, 4);
  partHeader.writeUInt32LE(part.material, 8);
  chunks.push(partHeader);

  const vertexBytes = Buffer.alloc(part.vertices.length * 8 * 4);
  let cursor = 0;
  for (const vertex of part.vertices) {
    for (const value of vertex) {
      vertexBytes.writeFloatLE(value, cursor);
      cursor += 4;
    }
  }
  chunks.push(vertexBytes);

  const indexBytes = Buffer.alloc(part.indices.length * 2);
  part.indices.forEach((value, index) => indexBytes.writeUInt16LE(value, index * 2));
  chunks.push(indexBytes);
}

fs.mkdirSync(outputDirectory, { recursive: true });
fs.writeFileSync(path.join(outputDirectory, 'sh3vr_lefthand.mesh'), Buffer.concat(chunks));

for (let materialIndex = 0; materialIndex < document.materials.length; ++materialIndex) {
  const textureIndex = document.materials[materialIndex]?.pbrMetallicRoughness
    ?.baseColorTexture?.index;
  if (!Number.isInteger(textureIndex)) continue;
  const imageIndex = document.textures[textureIndex].source;
  const image = document.images[imageIndex];
  if (image.mimeType !== 'image/png' || !Number.isInteger(image.bufferView)) {
    throw new Error(`material ${materialIndex} does not use an embedded PNG`);
  }
  const view = document.bufferViews[image.bufferView];
  const imageBytes = binary.subarray(view.byteOffset || 0,
    (view.byteOffset || 0) + view.byteLength);
  fs.writeFileSync(path.join(outputDirectory,
    `sh3vr_lefthand_${materialIndex}.png`), imageBytes);
}

console.log(`converted ${parts.length} parts, ${parts.reduce((n, p) => n + p.vertices.length, 0)} vertices, ` +
  `${parts.reduce((n, p) => n + p.indices.length, 0)} indices`);
