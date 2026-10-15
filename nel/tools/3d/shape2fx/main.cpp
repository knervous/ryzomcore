// shape2fx: a NeL CMesh .shape as JSON, for the Requiem effect library.
//
//   shape2fx <file.shape>            JSON on stdout; exit 1 on anything that is not a CMesh
//                                    or a CSegRemanenceShape (a weapon swing trail)
//
// Ryzom effects draw mesh particles (shockwave rings, rune circles, ice prisms, debris)
// from .shape files. shape2obj drops what an effect needs from them -- the material's
// texture and blend -- so this writes the triangles together with each render pass's
// material, read by NeL's own serialisation rather than a port of it.
//
// Coordinates are written as NeL stores them (Z up); the library builder converts.
//
// Built by tools/ryzom/build_nel_tools.sh, which copies this directory into the vendored
// NeL tree (vendor/ryzomcore is not tracked here).

#include <nel/misc/file.h>
#include <nel/misc/app_context.h>
#include <nel/3d/mesh.h>
#include <nel/3d/seg_remanence_shape.h>
#include <nel/3d/material.h>
#include <nel/3d/texture_file.h>
#include <nel/3d/scene.h>
#include <nel/3d/register_3d.h>

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

using namespace NLMISC;
using namespace NL3D;
using namespace std;

static string jsonString(const string &s)
{
	string out = "\"";
	for (char c : s)
	{
		if (c == '"' || c == '\\') { out += '\\'; out += c; }
		else if ((unsigned char)c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
		else out += c;
	}
	return out + "\"";
}

static string num(float v)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%.6g", v);
	return buf;
}

static string rgba(const CRGBA &c)
{
	return "[" + to_string(c.R) + "," + to_string(c.G) + "," + to_string(c.B) + "," + to_string(c.A) + "]";
}

static string textureName(ITexture *texture)
{
	CTextureFile *file = dynamic_cast<CTextureFile *>(texture);
	return file ? jsonString(file->getFileName()) : "null";
}

static string material(const CMaterial &m)
{
	return string("{")
		+ "\"shader\":" + to_string((int)m.getShader())
		+ ",\"texture\":" + textureName(m.getTexture(0))
		+ ",\"texture1\":" + textureName(m.getTexture(1))
		+ ",\"blend\":" + (m.getBlend() ? "true" : "false")
		+ ",\"srcBlend\":" + to_string((int)m.getSrcBlend())
		+ ",\"dstBlend\":" + to_string((int)m.getDstBlend())
		+ ",\"doubleSided\":" + (m.getDoubleSided() ? "true" : "false")
		+ ",\"zWrite\":" + (m.getZWrite() ? "true" : "false")
		+ ",\"alphaTest\":" + (m.getAlphaTest() ? "true" : "false")
		+ ",\"alphaTestThreshold\":" + num(m.getAlphaTestThreshold())
		+ ",\"diffuse\":" + rgba(m.getDiffuse())
		+ ",\"emissive\":" + rgba(m.getEmissive())
		+ "}";
}

static string vec(const CVector &v)
{
	return "[" + num(v.x) + "," + num(v.y) + "," + num(v.z) + "]";
}

// A swing trail: the blade's corner points, sampled in world space each frame by the
// runtime and strung into a ribbon (seg_remanence.cpp). Only the recipe lives here.
static int trail(CSegRemanenceShape *shape)
{
	ostringstream out;
	out << "{\"class\":\"CSegRemanenceShape\""
		<< ",\"numSlices\":" << shape->getNumSlices()
		<< ",\"sliceTime\":" << num(shape->getSliceTime())
		<< ",\"textureShifting\":" << (shape->getTextureShifting() ? "true" : "false")
		<< ",\"rollupRatio\":" << num(shape->getRollupRatio())
		<< ",\"corners\":[";
	for (uint i = 0; i < shape->getNumCorners(); ++i) out << (i ? "," : "") << vec(shape->getCorner(i));
	out << "],\"material\":" << material(shape->getMaterial());
	const CVector &pos = shape->getDefaultPos()->getDefaultValue();
	const CQuat &rot = shape->getDefaultRotQuat()->getDefaultValue();
	const CVector &scale = shape->getDefaultScale()->getDefaultValue();
	out << ",\"defaultPos\":" << vec(pos)
		<< ",\"defaultRotQuat\":[" << num(rot.x) << "," << num(rot.y) << "," << num(rot.z) << "," << num(rot.w) << "]"
		<< ",\"defaultScale\":" << vec(scale) << "}";
	cout << out.str() << endl;
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 2)
	{
		cerr << "usage: shape2fx <file.shape>" << endl;
		return 2;
	}
	if (!INelContext::isContextInitialised()) new CApplicationContext();
	registerSerial3d();
	CScene::registerBasics();

	CIFile in;
	if (!in.open(argv[1]))
	{
		cerr << "cannot open " << argv[1] << endl;
		return 1;
	}
	CShapeStream stream;
	try
	{
		stream.serial(in);
	}
	catch (const Exception &e)
	{
		cerr << "read failed: " << e.what() << endl;
		return 1;
	}
	if (CSegRemanenceShape *seg = dynamic_cast<CSegRemanenceShape *>(stream.getShapePointer())) return trail(seg);
	CMesh *mesh = dynamic_cast<CMesh *>(stream.getShapePointer());
	if (!mesh)
	{
		cerr << "not a CMesh" << endl;
		return 1;
	}

	const CMeshGeom &geom = mesh->getMeshGeom();
	const CVertexBuffer &vb = geom.getVertexBuffer();
	CVertexBufferRead vba;
	vb.lock(vba);
	const uint16 format = vb.getVertexFormat();
	const uint count = vb.getNumVertices();
	const bool hasNormal = (format & CVertexBuffer::NormalFlag) != 0;
	const bool hasUv = (format & CVertexBuffer::TexCoord0Flag) != 0;
	const bool hasColor = (format & CVertexBuffer::PrimaryColorFlag) != 0;
	const bool bgra = vb.getVertexColorFormat() == CVertexBuffer::TBGRA;

	ostringstream out;
	out << "{\"class\":\"CMesh\",\"vertexCount\":" << count;

	out << ",\"positions\":[";
	for (uint i = 0; i < count; ++i)
	{
		const CVector *p = vba.getVertexCoordPointer(i);
		out << (i ? "," : "") << num(p->x) << "," << num(p->y) << "," << num(p->z);
	}
	out << "]";
	if (hasNormal)
	{
		out << ",\"normals\":[";
		for (uint i = 0; i < count; ++i)
		{
			const CVector *n = vba.getNormalCoordPointer(i);
			out << (i ? "," : "") << num(n->x) << "," << num(n->y) << "," << num(n->z);
		}
		out << "]";
	}
	if (hasUv)
	{
		out << ",\"uvs\":[";
		for (uint i = 0; i < count; ++i)
		{
			const CUV *uv = vba.getTexCoordPointer(i, 0);
			out << (i ? "," : "") << num(uv->U) << "," << num(uv->V);
		}
		out << "]";
	}
	if (hasColor)
	{
		out << ",\"colors\":[";
		for (uint i = 0; i < count; ++i)
		{
			const uint8 *c = (const uint8 *)vba.getColorPointer(i);
			const uint r = bgra ? c[2] : c[0], b = bgra ? c[0] : c[2];
			out << (i ? "," : "") << r << "," << (uint)c[1] << "," << b << "," << (uint)c[3];
		}
		out << "]";
	}

	// Every matrix block's render passes: a pass is one material's triangles.
	out << ",\"passes\":[";
	bool firstPass = true;
	for (uint block = 0; block < geom.getNbMatrixBlock(); ++block)
	{
		for (uint pass = 0; pass < geom.getNbRdrPass(block); ++pass)
		{
			const CIndexBuffer &ib = geom.getRdrPassPrimitiveBlock(block, pass);
			CIndexBufferRead iba;
			ib.lock(iba);
			out << (firstPass ? "" : ",") << "{\"material\":" << geom.getRdrPassMaterial(block, pass) << ",\"indices\":[";
			firstPass = false;
			const uint n = ib.getNumIndexes();
			for (uint i = 0; i < n; ++i)
			{
				const uint index = iba.getFormat() == CIndexBuffer::Indices32
					? ((const uint32 *)iba.getPtr())[i]
					: ((const uint16 *)iba.getPtr())[i];
				out << (i ? "," : "") << index;
			}
			out << "]}";
		}
	}
	out << "]";

	out << ",\"materials\":[";
	for (uint i = 0; i < mesh->getNbMaterial(); ++i)
	{
		out << (i ? "," : "") << material(mesh->getMaterial(i));
	}
	out << "]";

	const CVector &pos = mesh->getDefaultPos()->getDefaultValue();
	const CVector &pivot = mesh->getDefaultPivot()->getDefaultValue();
	const CQuat &rot = mesh->getDefaultRotQuat()->getDefaultValue();
	const CVector &scale = mesh->getDefaultScale()->getDefaultValue();
	out << ",\"defaultPos\":[" << num(pos.x) << "," << num(pos.y) << "," << num(pos.z) << "]"
		<< ",\"defaultPivot\":[" << num(pivot.x) << "," << num(pivot.y) << "," << num(pivot.z) << "]"
		<< ",\"defaultRotQuat\":[" << num(rot.x) << "," << num(rot.y) << "," << num(rot.z) << "," << num(rot.w) << "]"
		<< ",\"defaultScale\":[" << num(scale.x) << "," << num(scale.y) << "," << num(scale.z) << "]"
		<< ",\"autoAnim\":" << (mesh->getAutoAnim() ? "true" : "false");

	const CAABBoxExt &box = geom.getBoundingBox();
	const CVector lo = box.getMin(), hi = box.getMax();
	out << ",\"bounds\":[" << num(lo.x) << "," << num(lo.y) << "," << num(lo.z) << ","
		<< num(hi.x) << "," << num(hi.y) << "," << num(hi.z) << "]";
	out << "}";

	cout << out.str() << endl;
	return 0;
}
