// NeL - MMORPG Framework <http://dev.ryzom.com/projects/nel/>
// Copyright (C) 2010  Winch Gate Property Limited
//
// This source file has been modified by the following contributors:
// Copyright (C) 2021  Jan BOON (Kaetemi) <jan.boon@kaetemi.be>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// Eltania intake patch (tools/patches/ryzomcore-shape2obj.patch):
//  * `shape2obj <input.shape> [outDir]` writes <stem>.obj + <stem>.mtl into outDir
//    (default: beside the input), plus <stem>.skin.json for skinned meshes.
//  * one `usemtl matN` group per render pass; the .mtl names each material's
//    stage-0 texture (map_Kd) and carries NeL render state as `# nel_*` comments.
//  * handles CMesh (all matrix blocks), CMeshMRM, CMeshMRMSkinned and
//    CMeshMultiLod (highest-detail non-coarse slot).
//  * vertex tables are sized from the vertex buffer, never from skin weights a
//    non-skinned CMeshMRM does not have.
//  * OBJ vertices are emitted in first-use order with v/vt/vn sharing one index,
//    so the skin.json rows line up 1:1 with the glTF vertices obj_gltf.ts builds.

#include <nel/misc/file.h>
#include <nel/3d/mesh.h>
#include <nel/3d/mesh_mrm.h>
#include <nel/3d/mesh_mrm_skinned.h>
#include <nel/3d/mesh_multi_lod.h>
#include <nel/3d/texture_file.h>
#include <nel/3d/texture_multi_file.h>
#include <nel/3d/scene.h>
#include <nel/3d/register_3d.h>
#include <nel/misc/app_context.h>
#include <nel/misc/path.h>

#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>

using namespace NLMISC;
using namespace NL3D;
using namespace std;

// ***************************************************************************

struct CPass
{
	CIndexBuffer	Block;		// copy: MRMSkinned only hands out copies
	uint32			MaterialId;
};

struct CGeomSource
{
	CVertexBuffer					VB;
	vector<CPass>					Passes;
	// VB index -> real vertex (MRM geomorphs map onto their End vertex).
	vector<uint32>					Geomorphs;
	vector<CMesh::CSkinWeight>		SkinWeights;
	vector<string>					BonesName;
};

static void readIndices(const CIndexBuffer &pb, vector<uint32> &out)
{
	out.clear();
	CIndexBufferRead iba;
	pb.lock(iba);
	uint n = pb.getNumIndexes();
	out.reserve(n);
	if (iba.getFormat() == CIndexBuffer::Indices32)
	{
		const uint32 *p = (const uint32 *)iba.getPtr();
		for (uint j = 0; j < n; ++j) out.push_back(p[j]);
	}
	else
	{
		const uint16 *p = (const uint16 *)iba.getPtr();
		for (uint j = 0; j < n; ++j) out.push_back(p[j]);
	}
}

static void gatherGeomorphs(const vector<CMRMWedgeGeom> &geomorphs, CGeomSource &src)
{
	src.Geomorphs.resize(geomorphs.size());
	for (size_t i = 0; i < geomorphs.size(); ++i) src.Geomorphs[i] = geomorphs[i].End;
}

static bool gatherMesh(const CMeshGeom *geom, CGeomSource &src)
{
	src.VB = geom->getVertexBuffer();
	for (uint mb = 0; mb < geom->getNbMatrixBlock(); ++mb)
	{
		for (uint p = 0; p < geom->getNbRdrPass(mb); ++p)
		{
			CPass pass;
			pass.Block = geom->getRdrPassPrimitiveBlock(mb, p);
			pass.MaterialId = geom->getRdrPassMaterial(mb, p);
			src.Passes.push_back(pass);
		}
	}
	return true;
}

static bool gatherMeshMRM(const CMeshMRMGeom *geom, CGeomSource &src)
{
	uint numLods = geom->getNbLod();
	if (!numLods) return false;
	uint lodId = numLods - 1;	// highest detail
	src.VB = geom->getVertexBuffer();
	for (uint p = 0; p < geom->getNbRdrPass(lodId); ++p)
	{
		CPass pass;
		pass.Block = geom->getRdrPassPrimitiveBlock(lodId, p);
		pass.MaterialId = geom->getRdrPassMaterial(lodId, p);
		src.Passes.push_back(pass);
	}
	gatherGeomorphs(geom->getGeomorphs(lodId), src);
	src.SkinWeights = geom->getSkinWeights();
	src.BonesName = geom->getBonesName();
	return true;
}

static bool gatherMeshMRMSkinned(const CMeshMRMSkinnedGeom *geom, CGeomSource &src)
{
	uint numLods = geom->getNbLod();
	if (!numLods) return false;
	uint lodId = numLods - 1;
	geom->getVertexBuffer(src.VB);
	for (uint p = 0; p < geom->getNbRdrPass(lodId); ++p)
	{
		CPass pass;
		geom->getRdrPassPrimitiveBlock(lodId, p, pass.Block);
		pass.MaterialId = geom->getRdrPassMaterial(lodId, p);
		src.Passes.push_back(pass);
	}
	gatherGeomorphs(geom->getGeomorphs(lodId), src);
	geom->getSkinWeights(src.SkinWeights);
	src.BonesName = geom->getBonesName();
	return true;
}

static bool gatherGeom(IMeshGeom *geom, CGeomSource &src)
{
	if (!geom) return false;
	if (CMeshMRMSkinnedGeom *g = dynamic_cast<CMeshMRMSkinnedGeom *>(geom)) return gatherMeshMRMSkinned(g, src);
	if (CMeshMRMGeom *g = dynamic_cast<CMeshMRMGeom *>(geom)) return gatherMeshMRM(g, src);
	if (CMeshGeom *g = dynamic_cast<CMeshGeom *>(geom)) return gatherMesh(g, src);
	return false;
}

// ***************************************************************************

static string textureName(const CMaterial &mat)
{
	ITexture *tex = mat.getTexture(0);
	if (!tex) return string();
	string name;
	if (CTextureFile *tf = dynamic_cast<CTextureFile *>(tex)) name = tf->getFileName();
	else if (CTextureMultiFile *tm = dynamic_cast<CTextureMultiFile *>(tex))
	{
		if (tm->getNumFileName() > 0) name = tm->getFileName(0);
	}
	return CFile::getFilename(name);
}

static void writeMtl(const string &path, CMeshBase *base, const vector<uint32> &usedMaterials)
{
	ofstream ofs(path.c_str());
	ofs << "# shape2obj (Eltania patch)" << endl;
	for (size_t k = 0; k < usedMaterials.size(); ++k)
	{
		uint32 id = usedMaterials[k];
		ofs << endl << "newmtl mat" << id << endl;
		if (!base || id >= base->getNbMaterial())
		{
			ofs << "Kd 1 1 1" << endl;
			continue;
		}
		const CMaterial &mat = base->getMaterial(id);
		string tex = textureName(mat);
		CRGBA diffuse = mat.getDiffuse();
		if (tex.empty())
			ofs << "Kd " << diffuse.R / 255.f << " " << diffuse.G / 255.f << " " << diffuse.B / 255.f << endl;
		else
			ofs << "Kd 1 1 1" << endl;
		if (mat.getBlend()) ofs << "d " << diffuse.A / 255.f << endl;
		if (!tex.empty()) ofs << "map_Kd " << tex << endl;
		if (mat.getBlend()) ofs << "# nel_blend " << (uint)mat.getSrcBlend() << " " << (uint)mat.getDstBlend() << endl;
		// After nel_blend: obj_gltf takes the last mode it sees, and a cutout reads better as MASK.
		if (mat.getAlphaTest()) ofs << "# nel_alpha_test " << mat.getAlphaTestThreshold() << endl;
		if (mat.getDoubleSided()) ofs << "# nel_double_sided" << endl;
		ofs << "# nel_shader " << (uint)mat.getShader() << endl;
	}
}

static int writeObj(const CGeomSource &src, CMeshBase *base, const string &outDir, const string &stem)
{
	CVertexBufferRead vba;
	src.VB.lock(vba);
	const uint numVerts = src.VB.getNumVertices();
	const uint16 fmt = src.VB.getVertexFormat();
	const bool hasNormal = (fmt & CVertexBuffer::NormalFlag) && src.VB.getValueType(CVertexBuffer::Normal) == CVertexBuffer::Float3;
	const bool hasUv = (fmt & CVertexBuffer::TexCoord0Flag) && src.VB.getValueType(CVertexBuffer::TexCoord0) == CVertexBuffer::Float2;
	const bool skinned = !src.BonesName.empty() && src.SkinWeights.size() >= numVerts && numVerts > 0;

	// VB index -> output index, assigned in first-use order (sized from the VB, not skin weights).
	vector<sint> outIndex(numVerts, -1);
	vector<uint32> outToVb;
	vector<uint32> indices;

	struct CGroup { uint32 Material; vector<uint32> Tris; };
	vector<CGroup> groups;
	vector<uint32> usedMaterials;

	for (size_t p = 0; p < src.Passes.size(); ++p)
	{
		readIndices(src.Passes[p].Block, indices);
		CGroup g;
		g.Material = src.Passes[p].MaterialId;
		for (size_t t = 0; t + 2 < indices.size(); t += 3)
		{
			uint32 tri[3];
			bool ok = true;
			for (uint c = 0; c < 3; ++c)
			{
				uint32 idx = indices[t + c];
				if (idx < src.Geomorphs.size()) idx = src.Geomorphs[idx];
				if (idx >= numVerts) { ok = false; break; }
				tri[c] = idx;
			}
			if (!ok) continue;
			for (uint c = 0; c < 3; ++c)
			{
				if (outIndex[tri[c]] < 0)
				{
					outIndex[tri[c]] = (sint)outToVb.size();
					outToVb.push_back(tri[c]);
				}
				g.Tris.push_back((uint32)outIndex[tri[c]]);
			}
		}
		if (g.Tris.empty()) continue;
		if (find(usedMaterials.begin(), usedMaterials.end(), g.Material) == usedMaterials.end())
			usedMaterials.push_back(g.Material);
		groups.push_back(g);
	}

	if (groups.empty())
	{
		cerr << "Error : shape has no triangles" << endl;
		return 1;
	}

	const string objPath = CPath::standardizePath(outDir) + stem + ".obj";
	const string mtlName = stem + ".mtl";
	ofstream ofs(objPath.c_str());
	ofs << setprecision(8);
	ofs << "mtllib " << mtlName << endl;
	for (size_t i = 0; i < outToVb.size(); ++i)
	{
		uint32 vi = outToVb[i];
		const CVector &v = *(const CVector *)vba.getVertexCoordPointer(vi);
		ofs << "v " << v.x << " " << v.y << " " << v.z << endl;
		if (hasNormal)
		{
			const CVector &n = *(const CVector *)vba.getNormalCoordPointer(vi);
			ofs << "vn " << n.x << " " << n.y << " " << n.z << endl;
		}
		if (hasUv)
		{
			const CUV &uv = *(const CUV *)vba.getTexCoordPointer(vi, 0);
			ofs << "vt " << uv.U << " " << (1.0f - uv.V) << endl;
		}
	}
	for (size_t gi = 0; gi < groups.size(); ++gi)
	{
		const CGroup &g = groups[gi];
		ofs << "g pass" << gi << endl;
		ofs << "usemtl mat" << g.Material << endl;
		for (size_t t = 0; t + 2 < g.Tris.size(); t += 3)
		{
			ofs << "f";
			for (uint c = 0; c < 3; ++c)
			{
				uint32 k = g.Tris[t + c] + 1;
				ofs << " " << k;
				if (hasUv && hasNormal) ofs << "/" << k << "/" << k;
				else if (hasUv) ofs << "/" << k;
				else if (hasNormal) ofs << "//" << k;
			}
			ofs << endl;
		}
	}
	ofs.close();

	writeMtl(CPath::standardizePath(outDir) + mtlName, base, usedMaterials);

	if (skinned)
	{
		ofstream sk((CPath::standardizePath(outDir) + stem + ".skin.json").c_str());
		sk << setprecision(7);
		sk << "{\"bones\":[";
		for (size_t b = 0; b < src.BonesName.size(); ++b)
		{
			if (b) sk << ",";
			sk << "\"";
			const string &n = src.BonesName[b];
			for (size_t c = 0; c < n.size(); ++c)
			{
				if (n[c] == '"' || n[c] == '\\') sk << '\\';
				sk << n[c];
			}
			sk << "\"";
		}
		sk << "],\"joints\":[";
		for (size_t i = 0; i < outToVb.size(); ++i)
		{
			const CMesh::CSkinWeight &w = src.SkinWeights[outToVb[i]];
			if (i) sk << ",";
			sk << "[";
			for (uint k = 0; k < NL3D_MESH_SKINNING_MAX_MATRIX; ++k)
				sk << (k ? "," : "") << (w.Weights[k] > 0 ? w.MatrixId[k] : 0);
			sk << "]";
		}
		sk << "],\"weights\":[";
		for (size_t i = 0; i < outToVb.size(); ++i)
		{
			const CMesh::CSkinWeight &w = src.SkinWeights[outToVb[i]];
			if (i) sk << ",";
			sk << "[";
			for (uint k = 0; k < NL3D_MESH_SKINNING_MAX_MATRIX; ++k)
				sk << (k ? "," : "") << w.Weights[k];
			sk << "]";
		}
		sk << "]}" << endl;
	}
	return 0;
}

// ***************************************************************************

int main(int argc, char *argv[])
{
	if (argc < 2)
	{
		cout << "Syntax : shape2obj <NeL .shape file> [output directory]" << endl;
		return 1;
	}

	if (!NLMISC::INelContext::isContextInitialised()) new NLMISC::CApplicationContext();

	registerSerial3d();
	CScene::registerBasics();

	string filename = argv[1];
	string outDir = argc >= 3 ? string(argv[2]) : CFile::getPath(filename);
	if (outDir.empty()) outDir = "./";
	string stem = CFile::getFilenameWithoutExtension(filename);

	CIFile ifile;
	if (!ifile.open(filename))
	{
		cerr << "Error : cannot open " << filename << endl;
		return 1;
	}

	IShape *shapeMesh = nullptr;
	CShapeStream streamShape;
	try
	{
		streamShape.serial(ifile);
		shapeMesh = streamShape.getShapePointer();
	}
	catch (const Exception &e)
	{
		cerr << "Error : " << e.what() << endl;
		return 1;
	}
	if (!shapeMesh)
	{
		cerr << "Error : no shape in stream" << endl;
		return 1;
	}

	CGeomSource src;
	CMeshBase *base = dynamic_cast<CMeshBase *>(shapeMesh);
	bool gathered = false;

	try
	{
		if (CMeshMRMSkinned *m = dynamic_cast<CMeshMRMSkinned *>(shapeMesh))
			gathered = gatherMeshMRMSkinned(&m->getMeshGeom(), src);
		else if (CMeshMRM *m = dynamic_cast<CMeshMRM *>(shapeMesh))
			gathered = gatherMeshMRM(&m->getMeshGeom(), src);
		else if (CMesh *m = dynamic_cast<CMesh *>(shapeMesh))
			gathered = gatherMesh(&m->getMeshGeom(), src);
		else if (CMeshMultiLod *m = dynamic_cast<CMeshMultiLod *>(shapeMesh))
		{
			// Slot 0 is the most detailed; coarse slots are impostor billboards.
			for (uint s = 0; s < m->getNumSlotMesh() && !gathered; ++s)
			{
				bool coarse = false;
				IMeshGeom *geom = m->getSlotMesh(s, coarse);
				if (coarse || !geom) continue;
				gathered = gatherGeom(geom, src);
			}
			if (!gathered)
			{
				cerr << "Error : CMeshMultiLod has no non-coarse slot mesh" << endl;
				return 1;
			}
		}
		else
		{
			cerr << "Error : unsupported shape class " << shapeMesh->getClassName() << endl;
			return 1;
		}
	}
	catch (const Exception &e)
	{
		cerr << "Error : " << e.what() << endl;
		return 1;
	}

	if (!gathered)
	{
		cerr << "Error : mesh has no LOD geometry" << endl;
		return 1;
	}

	return writeObj(src, base, outDir, stem);
}
