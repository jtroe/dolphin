// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <cmath>

#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoConfig.h"

static char text[16384];

template<class T>
static inline void GenerateGeometryShader(T& out, u32 components, API_TYPE ApiType)
{
	// Non-uid template parameters will write to the dummy data (=> gets optimized out)
	geometry_shader_uid_data dummy_data;
	geometry_shader_uid_data* uid_data = out.template GetUidData<geometry_shader_uid_data>();
	if (uid_data == nullptr)
		uid_data = &dummy_data;

	out.SetBuffer(text);
	const bool is_writing_shadercode = (out.GetBuffer() != nullptr);

	if (is_writing_shadercode)
		text[sizeof(text) - 1] = 0x7C;  // canary

	out.Write("//Geometry Shader for 3D stereoscopy\n");

	uid_data->stereo = g_ActiveConfig.iStereoMode > 0;
	if (ApiType == API_OPENGL)
	{
		// Insert layout parameters
		if (g_ActiveConfig.backend_info.bSupportsGSInstancing)
			out.Write("layout(triangles, invocations = %d) in;\n", g_ActiveConfig.iStereoMode > 0 ? 2 : 1);
		else
			out.Write("layout(triangles) in;\n");
		out.Write("layout(triangle_strip, max_vertices = %d) out;\n", g_ActiveConfig.backend_info.bSupportsGSInstancing ? 3 : 6);
	}

	out.Write("%s", s_lighting_struct);

	// uniforms
	if (ApiType == API_OPENGL)
		out.Write("layout(std140%s) uniform VSBlock {\n", g_ActiveConfig.backend_info.bSupportsBindingLayout ? ", binding = 2" : "");
	else
		out.Write("cbuffer VSBlock {\n");
	out.Write(s_shader_uniforms);
	out.Write("};\n");

	uid_data->numTexGens = xfmem.numTexGen.numTexGens;
	uid_data->pixel_lighting = g_ActiveConfig.bEnablePixelLighting;

	GenerateVSOutputStruct<T>(out, ApiType);

	if (ApiType == API_OPENGL)
	{
		out.Write("centroid in VS_OUTPUT o[3];\n");
		out.Write("centroid out VS_OUTPUT vs;\n");
		out.Write("flat out int layer;\n");

		out.Write("void main()\n{\n");
	}
	else // D3D
	{
		out.Write("struct GS_OUTPUT {\n");
		out.Write("\tVS_OUTPUT vs;\n");
		out.Write("\tuint layer : SV_RenderTargetArrayIndex;\n");
		out.Write("};\n");

		if (g_ActiveConfig.backend_info.bSupportsGSInstancing)
		{
			out.Write("[maxvertexcount(3)]\n[instance(%d)]\n", g_ActiveConfig.iStereoMode > 0 ? 2 : 1);
			out.Write("void main(triangle VS_OUTPUT o[3], inout TriangleStream<GS_OUTPUT> Output, in uint InstanceID : SV_GSInstanceID)\n{\n");
		}
		else
		{
			out.Write("[maxvertexcount(6)]\n");
			out.Write("void main(triangle VS_OUTPUT o[3], inout TriangleStream<GS_OUTPUT> Output)\n{\n");
		}

		out.Write("\tGS_OUTPUT gs;\n");
	}

	out.Write("\tVS_OUTPUT f;\n");

	// If the GPU supports invocation we don't need a for loop and can simply use the
	// invocation identifier to determine which layer we're rendering.
	if (g_ActiveConfig.backend_info.bSupportsGSInstancing)
	{
		if (ApiType == API_OPENGL)
			out.Write("\tint eye = gl_InvocationID;\n");
		else
			out.Write("\tint eye = InstanceID;\n");
	}
	else
		out.Write("\tfor (int eye = 0; eye < %d; ++eye) {\n", g_ActiveConfig.iStereoMode > 0 ? 2 : 1);

	out.Write("\tfor (int i = 0; i < 3; ++i) {\n");

	// Select the output layer
	if (ApiType == API_OPENGL)
	{
		out.Write("\t\tgl_Layer = eye;\n");
		out.Write("\t\tlayer = eye;\n");
	}
	else
		out.Write("\t\tgs.layer = eye;\n");

	out.Write("\t\tf = o[i];\n");
	out.Write("\t\tfloat4 pos = o[i].pos;\n");

	if (g_ActiveConfig.iStereoMode > 0)
	{
		// For stereoscopy add a small horizontal offset in Normalized Device Coordinates proportional
		// to the depth of the vertex. We retrieve the depth value from the w-component of the projected
		// vertex which contains the negated z-component of the original vertex.
		// For negative parallax (out-of-screen effects) we subtract a convergence value from
		// the depth value. This results in objects at a distance smaller than the convergence
		// distance to seemingly appear in front of the screen.
		// This formula is based on page 13 of the "Nvidia 3D Vision Automatic, Best Practices Guide"
		out.Write("\t\tf.clipPos.x = o[i].clipPos.x + " I_STEREOPARAMS"[eye] * (o[i].clipPos.w - " I_STEREOPARAMS"[2]);\n");
		out.Write("\t\tpos.x = o[i].pos.x + " I_STEREOPARAMS"[eye] * (o[i].pos.w - " I_STEREOPARAMS"[2]);\n");
	}

	out.Write("\t\tf.pos.x = pos.x;\n");

	if (ApiType == API_OPENGL)
		out.Write("\t\tgl_Position = pos;\n");

	out.Write("\t\t%s = f;\n", (ApiType == API_OPENGL) ? "vs" : "gs.vs");

	if (ApiType == API_OPENGL)
		out.Write("\t\tEmitVertex();\n");
	else
		out.Write("\t\tOutput.Append(gs);\n");

	out.Write("\t}\n");

	if (ApiType == API_OPENGL)
		out.Write("\tEndPrimitive();\n");
	else
		out.Write("\tOutput.RestartStrip();\n");

	if (!g_ActiveConfig.backend_info.bSupportsGSInstancing)
		out.Write("\t}\n");

	out.Write("}\n");

	if (is_writing_shadercode)
	{
		if (text[sizeof(text) - 1] != 0x7C)
			PanicAlert("GeometryShader generator - buffer too small, canary has been eaten!");
	}
}

void GetGeometryShaderUid(GeometryShaderUid& object, u32 components, API_TYPE ApiType)
{
	GenerateGeometryShader<GeometryShaderUid>(object, components, ApiType);
}

void GenerateGeometryShaderCode(ShaderCode& object, u32 components, API_TYPE ApiType)
{
	GenerateGeometryShader<ShaderCode>(object, components, ApiType);
}
