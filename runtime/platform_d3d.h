// platform_d3d.h - Direct3D 11 objects shared between the presenter and the hardware renderer.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

ID3D11Device* d3d_device();
ID3D11DeviceContext* d3d_context();
ID3DBlob* d3d_compile(const char* src, const char* entry, const char* target);
// draw a region of a texture to the window with the configured aspect ratio and filtering, then wait for the
// next vblank (frame pacing, input)
void d3d_present_texture(ID3D11ShaderResourceView* srv, float u0, float v0, float u1, float v1, int src_w, int src_h, int native_h, bool enabled);
// the same without the wait (in-between frames at an unlocked frame rate); false when minimized
bool d3d_show(ID3D11ShaderResourceView* srv, float u0, float v0, float u1, float v1, int src_w, int src_h, int native_h, bool enabled);
