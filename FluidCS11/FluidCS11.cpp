//--------------------------------------------------------------------------------------
// File: FluidCS11.cpp
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Smoothed Particle Hydrodynamics Algorithm Based Upon:
// Particle-Based Fluid Simulation for Interactive Applications
// Matthias M�ller
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Optimized Grid Algorithm Based Upon:
// Broad-Phase Collision Detection with CUDA
// Scott Le Grand
//--------------------------------------------------------------------------------------
#include "Macros.h"
#include "DXUT.h"
#include "DXUTsettingsDlg.h"
#include "SDKmisc.h"
#include "resource.h"
#include "WaitDlg.h"
#include <sstream> 
#include <algorithm>
#include "SSEDataTypes.h"

#pragma warning( disable : 4100 )

using namespace DirectX;

struct Particle
{
	XMFLOAT2 vPosition;
	XMFLOAT2 vVelocity;
};

struct ParticleDensity
{
	f32 fDensity;
};

struct ParticleForces
{
	XMFLOAT2 vAcceleration;
};

struct u322
{
	u32 x;
	u32 y;
};

//--------------------------------------------------------------------------------------
// Global variables
//--------------------------------------------------------------------------------------

// Compute Shader Constants
// Grid cell key size for sorting, 8-bits for x and y
const u64 NUM_GRID_INDICES = 65536;

// Numthreads size for the simulation
const u32 SIMULATION_BLOCK_SIZE = 256;

// Numthreads size for the sort
const u32 BITONIC_BLOCK_SIZE = 512;
const u32 TRANSPOSE_BLOCK_SIZE = 16;

// For this sample, only use power-of-2 numbers >= 8K and <= 64K
// The algorithm can be extended to support any number of particles
// But to keep the sample simple, we do not implement boundary conditions to handle it
const u32 NUM_PARTICLES_64K = 64 * 1024;
u32 g_iNumParticles = NUM_PARTICLES_64K;

// Particle Properties
// These will control how the fluid behaves
f32 g_fInitialParticleSpacing = 0.0045f;
f32 g_fSmoothlen = .012f;
f32 g_fPressureStiffness = 200.0f;
f32 g_fRestDensity = 1000.0f;
f32 g_fParticleMass = 0.0002f;
f32 g_fViscosity = 5.1f;
f32 g_fMaxAllowableTimeStep = 0.005f;
f32 g_fParticleRenderSize = 0.01f;

// Gravity Directions
const XMFLOAT2A GRAVITY_DOWN(0, -1.0f);
XMFLOAT2A g_vGravity = GRAVITY_DOWN;

// Map Size
// These values should not be larger than 256 * fSmoothlen
// Since the map must be divided up into fSmoothlen sized grid cells
// And the grid cell is used as a 16-bit sort key, 8-bits for x and y
f32 g_fMapHeight = 1.2f;
f32 g_fMapWidth = (4.0f / 3.0f) * g_fMapHeight;

// Map Wall Collision Planes
f32 g_fWallStiffness = 3000.0f;
XMFLOAT3A g_vPlanes[4] = {
	XMFLOAT3A(1, 0, 0),
	XMFLOAT3A(0, 1, 0),
	XMFLOAT3A(-1, 0, g_fMapWidth),
	XMFLOAT3A(0, -1, g_fMapHeight)
};

// Simulation Algorithm
enum eSimulationMode
{
	SIM_MODE_SIMPLE,
	SIM_MODE_SHARED,
	SIM_MODE_GRID
};

eSimulationMode g_eSimMode = SIM_MODE_GRID;



XMFLOAT2A mousePosition;
//--------------------------------------------------------------------------------------
// Direct3D11 Global variables
//--------------------------------------------------------------------------------------
ID3D11ShaderResourceView * const     g_pNullSRV = nullptr;       // Helper to Clear SRVs
ID3D11UnorderedAccessView* const    g_pNullUAV = nullptr;       // Helper to Clear UAVs
ID3D11Buffer* const                 g_pNullBuffer = nullptr;    // Helper to Clear Buffers
u32                                g_iNullu32 = 0;         // Helper to Clear Buffers

// Shaders
ID3D11VertexShader*                 g_pParticleVS = nullptr;
ID3D11GeometryShader*               g_pParticleGS = nullptr;
ID3D11PixelShader*                  g_pParticlePS = nullptr;

ID3D11ComputeShader*                g_pBuildGridCS = nullptr;
ID3D11ComputeShader*                g_pClearGridIndicesCS = nullptr;
ID3D11ComputeShader*                g_pBuildGridIndicesCS = nullptr;
ID3D11ComputeShader*                g_pRearrangeParticlesCS = nullptr;
//ID3D11ComputeShader*				g_pRepositionParticlesCS = nullptr;
ID3D11ComputeShader*                g_pDensity_SimpleCS = nullptr;
ID3D11ComputeShader*                g_pForce_SimpleCS = nullptr;
ID3D11ComputeShader*                g_pDensity_SharedCS = nullptr;
ID3D11ComputeShader*                g_pForce_SharedCS = nullptr;
ID3D11ComputeShader*                g_pDensity_GridCS = nullptr;
ID3D11ComputeShader*                g_pForce_GridCS = nullptr;
ID3D11ComputeShader*                g_pIntegrateCS = nullptr;

ID3D11ComputeShader*                g_pSortBitonic = nullptr;
ID3D11ComputeShader*                g_pSortTranspose = nullptr;

// Structured Buffers
ID3D11Buffer*                       g_pParticles = nullptr;
ID3D11ShaderResourceView*           g_pParticlesSRV = nullptr;
ID3D11UnorderedAccessView*          g_pParticlesUAV = nullptr;

ID3D11Buffer*                       g_pSortedParticles = nullptr;
ID3D11ShaderResourceView*           g_pSortedParticlesSRV = nullptr;
ID3D11UnorderedAccessView*          g_pSortedParticlesUAV = nullptr;

ID3D11Buffer*                       g_pParticleDensity = nullptr;
ID3D11ShaderResourceView*           g_pParticleDensitySRV = nullptr;
ID3D11UnorderedAccessView*          g_pParticleDensityUAV = nullptr;
 
ID3D11Buffer*                       g_pParticleForces = nullptr;
ID3D11ShaderResourceView*           g_pParticleForcesSRV = nullptr;
ID3D11UnorderedAccessView*          g_pParticleForcesUAV = nullptr;

ID3D11Buffer*                       g_pGrid = nullptr;
ID3D11ShaderResourceView*           g_pGridSRV = nullptr;
ID3D11UnorderedAccessView*          g_pGridUAV = nullptr;

ID3D11Buffer*                       g_pGridPingPong = nullptr;
ID3D11ShaderResourceView*           g_pGridPingPongSRV = nullptr;
ID3D11UnorderedAccessView*          g_pGridPingPongUAV = nullptr;

ID3D11Buffer*                       g_pGridIndices = nullptr;
ID3D11ShaderResourceView*           g_pGridIndicesSRV = nullptr;
ID3D11UnorderedAccessView*          g_pGridIndicesUAV = nullptr;
//
//ID3D11UnorderedAccessView*			m_pRepositionUAVs = nullptr;
//ID3D11ShaderResourceView*			m_pRepositionSRVs = nullptr;
//ID3D11Buffer*						m_pRepositionBuffers = nullptr;

// Constant Buffer Layout
#pragma warning(push)
#pragma warning(disable:4324) // structure was padded due to __declspec(align())
__declspec(align(16)) struct CBSimulationConstants
{
	u32 iNumParticles;
	f32 fTimeStep;
	f32 fSmoothlen;
	f32 fPressureStiffness;
	f32 fRestDensity;
	f32 fDensityCoef;
	f32 fGradPressureCoef;
	f32 fLapViscosityCoef;
	f32 fWallStiffness;

	XMFLOAT2A vGravity;
	XMFLOAT4A vGridDim;

	XMFLOAT3A vPlanes[4];
	XMFLOAT2A mousePosition;
};

__declspec(align(16)) struct CBRenderConstants
{
	XMFLOAT4X4 mViewProjection;
	f32 fParticleSize;
};

__declspec(align(16)) struct SortCB
{
	u32 iLevel;
	u32 iLevelMask;
	u32 iWidth;
	u32 iHeight;
};
#pragma warning(pop)

// Constant Buffers
ID3D11Buffer*                       g_pcbSimulationConstants = nullptr;
ID3D11Buffer*                       g_pcbRenderConstants = nullptr;
ID3D11Buffer*                       g_pSortCB = nullptr;


//--------------------------------------------------------------------------------------
// Forward declarations 
//--------------------------------------------------------------------------------------
bool CALLBACK ModifyDeviceSettings(DXUTDeviceSettings* pDeviceSettings, void* pUserContext);
LRESULT CALLBACK MsgProc(HWND hWnd, u32 uMsg, WPARAM wParam, LPARAM lParam, bool* pbNoFurtherProcessing,
	void* pUserContext);
void CALLBACK OnGUIEvent(u32 nEvent, i32 nControlID, CDXUTControl* pControl, void* pUserContext);

bool CALLBACK IsD3D11DeviceAcceptable(const CD3D11EnumAdapterInfo *AdapterInfo, u32 Output, const CD3D11EnumDeviceInfo *DeviceInfo,
	DXGI_FORMAT BackBufferFormat, bool bWindowed, void* pUserContext);
HRESULT CALLBACK OnD3D11CreateDevice(ID3D11Device* pd3dDevice, const DXGI_SURFACE_DESC* pBackBufferSurfaceDesc,
	void* pUserContext);
HRESULT CALLBACK OnD3D11ResizedSwapChain(ID3D11Device* pd3dDevice, IDXGISwapChain* pSwapChain,
	const DXGI_SURFACE_DESC* pBackBufferSurfaceDesc, void* pUserContext);
void CALLBACK OnD3D11ReleasingSwapChain(void* pUserContext);
void CALLBACK OnD3D11DestroyDevice(void* pUserContext);
void CALLBACK OnD3D11FrameRender(ID3D11Device* pd3dDevice, ID3D11DeviceContext* pd3dImmediateContext, double fTime,
	f32 fElapsedTime, void* pUserContext);

HRESULT CreateSimulationBuffers(ID3D11Device* pd3dDevice);
void InitApp();
void RenderText();

//--------------------------------------------------------------------------------------
// Entry point to the program. Initializes everything and goes into a message processing 
// loop. Idle time is used to render the scene.
//--------------------------------------------------------------------------------------
i32 WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ i32 nCmdShow)
{
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	// DXUT will create and use the best device
	// that is available on the system depending on which D3D callbacks are set below

	// Set DXUT callbacks
	DXUTSetCallbackDeviceChanging(ModifyDeviceSettings);
	DXUTSetCallbackMsgProc(MsgProc);

	DXUTSetCallbackD3D11DeviceAcceptable(IsD3D11DeviceAcceptable);
	DXUTSetCallbackD3D11DeviceCreated(OnD3D11CreateDevice);
	DXUTSetCallbackD3D11SwapChainResized(OnD3D11ResizedSwapChain);
	DXUTSetCallbackD3D11FrameRender(OnD3D11FrameRender);
	DXUTSetCallbackD3D11SwapChainReleasing(OnD3D11ReleasingSwapChain);
	DXUTSetCallbackD3D11DeviceDestroyed(OnD3D11DestroyDevice);

	InitApp();
	DXUTInit(true, true); // Parse the command line, show msgboxes on error, and an extra cmd line param to force REF for now
	DXUTSetCursorSettings(true, true); // Show the cursor and clip it when in full screen
	DXUTCreateWindow(L"FluidCS11");
	DXUTCreateDevice(D3D_FEATURE_LEVEL_10_0, true, 1024, 768);
	DXUTMainLoop(); // Enter into the DXUT render loop

	return DXUTGetExitCode();
}


//--------------------------------------------------------------------------------------
// Initialize the app 
//--------------------------------------------------------------------------------------
void InitApp()
{
	DXUTInit();
}


//--------------------------------------------------------------------------------------
// Called right before creating a D3D device, allowing the app to modify the device settings as needed
//--------------------------------------------------------------------------------------
bool CALLBACK ModifyDeviceSettings(DXUTDeviceSettings* pDeviceSettings, void* pUserContext)
{
	return true;
}


//--------------------------------------------------------------------------------------
// Render the help and statistics text
//--------------------------------------------------------------------------------------
void RenderText()
{
	std::wostringstream outs;
	outs << "FPS : ";
	outs << DXUTGetFPS();
	outs << " Stats : ";
	outs << DXUTGetDeviceStats();
	outs << " FrameStats : ";
	outs << DXUTGetFrameStats();
	SetWindowText( GetActiveWindow(), outs.str().c_str() );
}


//--------------------------------------------------------------------------------------
// Handle messages to the application
//--------------------------------------------------------------------------------------
LRESULT CALLBACK MsgProc(HWND hWnd, u32 uMsg, WPARAM wParam, LPARAM lParam, bool* pbNoFurtherProcessing,
	void* pUserContext)
{	
	return 0;
}



//--------------------------------------------------------------------------------------
// Reject any D3D11 devices that aren't acceptable by returning false
//--------------------------------------------------------------------------------------
bool CALLBACK IsD3D11DeviceAcceptable(const CD3D11EnumAdapterInfo *AdapterInfo, u32 Output, const CD3D11EnumDeviceInfo *DeviceInfo,
	DXGI_FORMAT BackBufferFormat, bool bWindowed, void* pUserContext)
{
	if (DeviceInfo->ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x == FALSE)
		return false;

	return true;
}


//--------------------------------------------------------------------------------------
// Helper for creating constant buffers
//--------------------------------------------------------------------------------------
template <class T>
HRESULT CreateConstantBuffer(ID3D11Device* pd3dDevice, ID3D11Buffer** ppCB)
{
	HRESULT hr = S_OK;

	D3D11_BUFFER_DESC Desc;
	Desc.Usage = D3D11_USAGE_DEFAULT;
	Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	Desc.CPUAccessFlags = 0;
	Desc.MiscFlags = 0;
	Desc.ByteWidth = sizeof(T);
	V_RETURN(pd3dDevice->CreateBuffer(&Desc, nullptr, ppCB));

	return hr;
}


//--------------------------------------------------------------------------------------
// Helper for creating structured buffers with an SRV and UAV
//--------------------------------------------------------------------------------------
template <class T>
HRESULT CreateStructuredBuffer(ID3D11Device* pd3dDevice, u32 iNumElements, ID3D11Buffer** ppBuffer, ID3D11ShaderResourceView** ppSRV, ID3D11UnorderedAccessView** ppUAV, const T* pInitialData = nullptr)
{
	HRESULT hr = S_OK;

	// Create SB
	D3D11_BUFFER_DESC bufferDesc;
	ZeroMemory(&bufferDesc, sizeof(bufferDesc));
	bufferDesc.ByteWidth = iNumElements * sizeof(T);
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bufferDesc.StructureByteStride = sizeof(T);

	D3D11_SUBRESOURCE_DATA bufferInitData;
	ZeroMemory(&bufferInitData, sizeof(bufferInitData));
	bufferInitData.pSysMem = pInitialData;
	V_RETURN(pd3dDevice->CreateBuffer(&bufferDesc, (pInitialData) ? &bufferInitData : nullptr, ppBuffer));

	// Create SRV
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	ZeroMemory(&srvDesc, sizeof(srvDesc));
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.ElementWidth = iNumElements;
	V_RETURN(pd3dDevice->CreateShaderResourceView(*ppBuffer, &srvDesc, ppSRV));

	// Create UAV
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc;
	ZeroMemory(&uavDesc, sizeof(uavDesc));
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.NumElements = iNumElements;
	V_RETURN(pd3dDevice->CreateUnorderedAccessView(*ppBuffer, &uavDesc, ppUAV));

	return hr;
}


//--------------------------------------------------------------------------------------
// Create the buffers used for the simulation data
//--------------------------------------------------------------------------------------
HRESULT CreateSimulationBuffers(ID3D11Device* pd3dDevice)
{
	HRESULT hr = S_OK;

	// Destroy the old buffers in case the number of particles has changed
	SAFE_RELEASE(g_pParticles);
	SAFE_RELEASE(g_pParticlesSRV);
	SAFE_RELEASE(g_pParticlesUAV);

	SAFE_RELEASE(g_pSortedParticles);
	SAFE_RELEASE(g_pSortedParticlesSRV);
	SAFE_RELEASE(g_pSortedParticlesUAV);

	SAFE_RELEASE(g_pParticleForces);
	SAFE_RELEASE(g_pParticleForcesSRV);
	SAFE_RELEASE(g_pParticleForcesUAV);

	SAFE_RELEASE(g_pParticleDensity);
	SAFE_RELEASE(g_pParticleDensitySRV);
	SAFE_RELEASE(g_pParticleDensityUAV);

	SAFE_RELEASE(g_pGridSRV);
	SAFE_RELEASE(g_pGridUAV);
	SAFE_RELEASE(g_pGrid);

	SAFE_RELEASE(g_pGridPingPongSRV);
	SAFE_RELEASE(g_pGridPingPongUAV);
	SAFE_RELEASE(g_pGridPingPong);

	SAFE_RELEASE(g_pGridIndicesSRV);
	SAFE_RELEASE(g_pGridIndicesUAV);
	SAFE_RELEASE(g_pGridIndices);

	// Create the initial particle positions
	// This is only used to populate the GPU buffers on creation
	const u32 iStartingWidth = (u32)sqrt((FLOAT)g_iNumParticles);
	Particle* particles = new Particle[g_iNumParticles];
	ZeroMemory(particles, sizeof(Particle) * g_iNumParticles);
	for (u32 i = 0; i < g_iNumParticles; i++)
	{
		// Arrange the particles in a nice square
		u32 x = i % iStartingWidth;
		u32 y = i / iStartingWidth;
		particles[i].vPosition = XMFLOAT2(g_fInitialParticleSpacing * (FLOAT)x, g_fInitialParticleSpacing * (FLOAT)y);
	}

	// Create Structured Buffers
	V_RETURN(CreateStructuredBuffer< Particle >(pd3dDevice, g_iNumParticles, &g_pParticles, &g_pParticlesSRV, &g_pParticlesUAV, particles));
	DXUT_SetDebugName(g_pParticles, "Particles");
	DXUT_SetDebugName(g_pParticlesSRV, "Particles SRV");
	DXUT_SetDebugName(g_pParticlesUAV, "Particles UAV");

	V_RETURN(CreateStructuredBuffer< Particle >(pd3dDevice, g_iNumParticles, &g_pSortedParticles, &g_pSortedParticlesSRV, &g_pSortedParticlesUAV, particles));
	DXUT_SetDebugName(g_pSortedParticles, "Sorted");
	DXUT_SetDebugName(g_pSortedParticlesSRV, "Sorted SRV");
	DXUT_SetDebugName(g_pSortedParticlesUAV, "Sorted UAV");

	V_RETURN(CreateStructuredBuffer< ParticleForces >(pd3dDevice, g_iNumParticles, &g_pParticleForces, &g_pParticleForcesSRV, &g_pParticleForcesUAV));
	DXUT_SetDebugName(g_pParticleForces, "Forces");
	DXUT_SetDebugName(g_pParticleForcesSRV, "Forces SRV");
	DXUT_SetDebugName(g_pParticleForcesUAV, "Forces UAV");

	V_RETURN(CreateStructuredBuffer< ParticleDensity >(pd3dDevice, g_iNumParticles, &g_pParticleDensity, &g_pParticleDensitySRV, &g_pParticleDensityUAV));
	DXUT_SetDebugName(g_pParticleDensity, "Density");
	DXUT_SetDebugName(g_pParticleDensitySRV, "Density SRV");
	DXUT_SetDebugName(g_pParticleDensityUAV, "Density UAV");

	V_RETURN(CreateStructuredBuffer< u32 >(pd3dDevice, g_iNumParticles, &g_pGrid, &g_pGridSRV, &g_pGridUAV));
	DXUT_SetDebugName(g_pGrid, "Grid");
	DXUT_SetDebugName(g_pGridSRV, "Grid SRV");
	DXUT_SetDebugName(g_pGridUAV, "Grid UAV");

	V_RETURN(CreateStructuredBuffer< u32 >(pd3dDevice, g_iNumParticles, &g_pGridPingPong, &g_pGridPingPongSRV, &g_pGridPingPongUAV));
	DXUT_SetDebugName(g_pGridPingPong, "PingPong");
	DXUT_SetDebugName(g_pGridPingPongSRV, "PingPong SRV");
	DXUT_SetDebugName(g_pGridPingPongUAV, "PingPong UAV");

	V_RETURN(CreateStructuredBuffer< u322 >(pd3dDevice, NUM_GRID_INDICES, &g_pGridIndices, &g_pGridIndicesSRV, &g_pGridIndicesUAV));
	DXUT_SetDebugName(g_pGridIndices, "Indices");
	DXUT_SetDebugName(g_pGridIndicesSRV, "Indices SRV");
	DXUT_SetDebugName(g_pGridIndicesUAV, "Indices UAV");

	//V_RETURN(CreateStructuredBuffer < Particle >(pd3dDevice, g_iNumParticles, &m_pRepositionBuffers, &m_pRepositionSRVs, &m_pRepositionUAVs));
	//DXUT_SetDebugName(m_pRepositionBuffers, "reposition buffers");
	//DXUT_SetDebugName(m_pRepositionSRVs, "reposition SRV");
	//DXUT_SetDebugName(m_pRepositionUAVs, "reposition UAV");

	delete[] particles;

	return S_OK;
}


//--------------------------------------------------------------------------------------
// Create any D3D11 resources that aren't dependant on the back buffer
//--------------------------------------------------------------------------------------
HRESULT CALLBACK OnD3D11CreateDevice(ID3D11Device* pd3dDevice, const DXGI_SURFACE_DESC* pBackBufferSurfaceDesc,
	void* pUserContext)
{
	HRESULT hr;

	auto pd3dImmediateContext = DXUTGetD3D11DeviceContext();

	// Compile the Shaders
	ID3DBlob* pBlob = nullptr;

	// Rendering Shaders
	V_RETURN(DXUTCompileFromFile(L"FluidRender.hlsl", nullptr, "ParticleVS", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateVertexShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pParticleVS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pParticleVS, "ParticleVS");

	V_RETURN(DXUTCompileFromFile(L"FluidRender.hlsl", nullptr, "ParticleGS", "gs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateGeometryShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pParticleGS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pParticleGS, "ParticleGS");

	V_RETURN(DXUTCompileFromFile(L"FluidRender.hlsl", nullptr, "ParticlePS", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreatePixelShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pParticlePS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pParticlePS, "ParticlePS");

	// Compute Shaders
	const char* CSTarget = (pd3dDevice->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0) ? "cs_5_0" : "cs_4_0";

	CWaitDlg CompilingShadersDlg;
	CompilingShadersDlg.ShowDialog(L"Compiling Shaders...");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "IntegrateCS", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pIntegrateCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pIntegrateCS, "IntegrateCS");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "DensityCS_Simple", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pDensity_SimpleCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pDensity_SimpleCS, "DensityCS_Simple");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "ForceCS_Simple", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pForce_SimpleCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pForce_SimpleCS, "ForceCS_Simple");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "DensityCS_Shared", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pDensity_SharedCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pDensity_SharedCS, "DensityCS_Shared");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "ForceCS_Shared", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pForce_SharedCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pForce_SharedCS, "ForceCS_Shared");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "DensityCS_Grid", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pDensity_GridCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pDensity_GridCS, "DensityCS_Grid");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "ForceCS_Grid", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pForce_GridCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pForce_GridCS, "ForceCS_Grid");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "BuildGridCS", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pBuildGridCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pBuildGridCS, "BuildGridCS");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "ClearGridIndicesCS", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pClearGridIndicesCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pClearGridIndicesCS, "ClearGridIndicesCS");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "BuildGridIndicesCS", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pBuildGridIndicesCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pBuildGridIndicesCS, "BuildGridIndicesCS");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "RearrangeParticlesCS", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pRearrangeParticlesCS));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pRearrangeParticlesCS, "RearrangeParticlesCS");

	V_RETURN(DXUTCompileFromFile(L"FluidCS11.hlsl", nullptr, "RepositionCS", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	//V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pRepositionParticlesCS));
	//SAFE_RELEASE(pBlob);
	//DXUT_SetDebugName(g_pRepositionParticlesCS, "RepositionParticlesCS");



	// Sort Shaders
	V_RETURN(DXUTCompileFromFile(L"ComputeShaderSort11.hlsl", nullptr, "BitonicSort", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pSortBitonic));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pSortBitonic, "BitonicSort");

	V_RETURN(DXUTCompileFromFile(L"ComputeShaderSort11.hlsl", nullptr, "MatrixTranspose", CSTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, &pBlob));
	V_RETURN(pd3dDevice->CreateComputeShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_pSortTranspose));
	SAFE_RELEASE(pBlob);
	DXUT_SetDebugName(g_pSortTranspose, "MatrixTranspose");

	CompilingShadersDlg.DestroyDialog();

	// Create the Simulation Buffers
	V_RETURN(CreateSimulationBuffers(pd3dDevice));

	// Create Constant Buffers
	V_RETURN(CreateConstantBuffer< CBSimulationConstants >(pd3dDevice, &g_pcbSimulationConstants));
	V_RETURN(CreateConstantBuffer< CBRenderConstants >(pd3dDevice, &g_pcbRenderConstants));

	V_RETURN(CreateConstantBuffer< SortCB >(pd3dDevice, &g_pSortCB));

	return S_OK;
}


//--------------------------------------------------------------------------------------
// Create any D3D11 resources that depend on the back buffer
//--------------------------------------------------------------------------------------
HRESULT CALLBACK OnD3D11ResizedSwapChain(ID3D11Device* pd3dDevice, IDXGISwapChain* pSwapChain,
	const DXGI_SURFACE_DESC* pBackBufferSurfaceDesc, void* pUserContext)
{
	HRESULT hr;
	return S_OK;
}


//--------------------------------------------------------------------------------------
// GPU Bitonic Sort
// For more information, please see the ComputeShaderSort11 sample
//--------------------------------------------------------------------------------------
void GPUSort(ID3D11DeviceContext* pd3dImmediateContext,
	ID3D11UnorderedAccessView* inUAV, ID3D11ShaderResourceView* inSRV,
	ID3D11UnorderedAccessView* tempUAV, ID3D11ShaderResourceView* tempSRV)
{
	pd3dImmediateContext->CSSetConstantBuffers(0, 1, &g_pSortCB);

	const u32 NUM_ELEMENTS = g_iNumParticles;
	const u32 MATRIX_WIDTH = BITONIC_BLOCK_SIZE;
	const u32 MATRIX_HEIGHT = NUM_ELEMENTS / BITONIC_BLOCK_SIZE;

	// Sort the data
	// First sort the rows for the levels <= to the block size
	for (u32 level = 2; level <= BITONIC_BLOCK_SIZE; level <<= 1)
	{
		SortCB constants = { level, level, MATRIX_HEIGHT, MATRIX_WIDTH };
		pd3dImmediateContext->UpdateSubresource(g_pSortCB, 0, nullptr, &constants, 0, 0);

		// Sort the row data
		u32 UAVInitialCounts = 0;
		pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &inUAV, &UAVInitialCounts);
		pd3dImmediateContext->CSSetShader(g_pSortBitonic, nullptr, 0);
		pd3dImmediateContext->Dispatch(NUM_ELEMENTS / BITONIC_BLOCK_SIZE, 1, 1);
	}

	// Then sort the rows and columns for the levels > than the block size
	// Transpose. Sort the Columns. Transpose. Sort the Rows.
	for (u32 level = (BITONIC_BLOCK_SIZE << 1); level <= NUM_ELEMENTS; level <<= 1)
	{
		SortCB constants1 = { (level / BITONIC_BLOCK_SIZE), (level & ~NUM_ELEMENTS) / BITONIC_BLOCK_SIZE, MATRIX_WIDTH, MATRIX_HEIGHT };
		pd3dImmediateContext->UpdateSubresource(g_pSortCB, 0, nullptr, &constants1, 0, 0);

		// Transpose the data from buffer 1 into buffer 2
		ID3D11ShaderResourceView* pViewNULL = nullptr;
		u32 UAVInitialCounts = 0;
		pd3dImmediateContext->CSSetShaderResources(0, 1, &pViewNULL);
		pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &tempUAV, &UAVInitialCounts);
		pd3dImmediateContext->CSSetShaderResources(0, 1, &inSRV);
		pd3dImmediateContext->CSSetShader(g_pSortTranspose, nullptr, 0);
		pd3dImmediateContext->Dispatch(MATRIX_WIDTH / TRANSPOSE_BLOCK_SIZE, MATRIX_HEIGHT / TRANSPOSE_BLOCK_SIZE, 1);

		// Sort the transposed column data
		pd3dImmediateContext->CSSetShader(g_pSortBitonic, nullptr, 0);
		pd3dImmediateContext->Dispatch(NUM_ELEMENTS / BITONIC_BLOCK_SIZE, 1, 1);

		SortCB constants2 = { BITONIC_BLOCK_SIZE, level, MATRIX_HEIGHT, MATRIX_WIDTH };
		pd3dImmediateContext->UpdateSubresource(g_pSortCB, 0, nullptr, &constants2, 0, 0);

		// Transpose the data from buffer 2 back into buffer 1
		pd3dImmediateContext->CSSetShaderResources(0, 1, &pViewNULL);
		pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &inUAV, &UAVInitialCounts);
		pd3dImmediateContext->CSSetShaderResources(0, 1, &tempSRV);
		pd3dImmediateContext->CSSetShader(g_pSortTranspose, nullptr, 0);
		pd3dImmediateContext->Dispatch(MATRIX_HEIGHT / TRANSPOSE_BLOCK_SIZE, MATRIX_WIDTH / TRANSPOSE_BLOCK_SIZE, 1);

		// Sort the row data
		pd3dImmediateContext->CSSetShader(g_pSortBitonic, nullptr, 0);
		pd3dImmediateContext->Dispatch(NUM_ELEMENTS / BITONIC_BLOCK_SIZE, 1, 1);
	}
}


//--------------------------------------------------------------------------------------
// GPU Fluid Simulation - Simple N^2 Algorithm
//--------------------------------------------------------------------------------------
void SimulateFluid_Simple(ID3D11DeviceContext* pd3dImmediateContext)
{
	u32 UAVInitialCounts = 0;

	// Setup
	pd3dImmediateContext->CSSetConstantBuffers(0, 1, &g_pcbSimulationConstants);
	pd3dImmediateContext->CSSetShaderResources(0, 1, &g_pParticlesSRV);

	// Density
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pParticleDensityUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShader(g_pDensity_SimpleCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);

	// Force
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pParticleForcesUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(1, 1, &g_pParticleDensitySRV);
	pd3dImmediateContext->CSSetShader(g_pForce_SimpleCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);

	// Integrate
	pd3dImmediateContext->CopyResource(g_pSortedParticles, g_pParticles);
	pd3dImmediateContext->CSSetShaderResources(0, 1, &g_pSortedParticlesSRV);
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pParticlesUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(2, 1, &g_pParticleForcesSRV);
	pd3dImmediateContext->CSSetShader(g_pIntegrateCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);
}


//--------------------------------------------------------------------------------------
// GPU Fluid Simulation - Optimized N^2 Algorithm using Shared Memory
//--------------------------------------------------------------------------------------
void SimulateFluid_Shared(ID3D11DeviceContext* pd3dImmediateContext)
{
	u32 UAVInitialCounts = 0;

	// Setup
	pd3dImmediateContext->CSSetConstantBuffers(0, 1, &g_pcbSimulationConstants);
	pd3dImmediateContext->CSSetShaderResources(0, 1, &g_pParticlesSRV);

	// Density
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pParticleDensityUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShader(g_pDensity_SharedCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);

	// Force
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pParticleForcesUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(1, 1, &g_pParticleDensitySRV);
	pd3dImmediateContext->CSSetShader(g_pForce_SharedCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);

	// Integrate
	pd3dImmediateContext->CopyResource(g_pSortedParticles, g_pParticles);
	pd3dImmediateContext->CSSetShaderResources(0, 1, &g_pSortedParticlesSRV);
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pParticlesUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(2, 1, &g_pParticleForcesSRV);
	pd3dImmediateContext->CSSetShader(g_pIntegrateCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);
}


//--------------------------------------------------------------------------------------
// GPU Fluid Simulation - Optimized Algorithm using a Grid + Sort
// Algorithm Overview:
//    Build Grid: For every particle, calculate a hash based on the grid cell it is in
//    Sort Grid: Sort all of the particles based on the grid ID hash
//        Particles in the same cell will now be adjacent in memory
//    Build Grid Indices: Located the start and end offsets for each cell
//    Rearrange: Rearrange the particles into the same order as the grid for easy lookup
//    Density, Force, Integrate: Perform the normal fluid simulation algorithm
//        Except now, only calculate particles from the 8 adjacent cells + current cell
//--------------------------------------------------------------------------------------
void SimulateFluid_Grid(ID3D11DeviceContext* pd3dImmediateContext)
{
	u32 UAVInitialCounts = 0;

	// Setup
	pd3dImmediateContext->CSSetConstantBuffers(0, 1, &g_pcbSimulationConstants);
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pGridUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(0, 1, &g_pParticlesSRV);
	// Build Grid
	pd3dImmediateContext->CSSetShader(g_pBuildGridCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);

	// Sort Grid
	GPUSort(pd3dImmediateContext, g_pGridUAV, g_pGridSRV, g_pGridPingPongUAV, g_pGridPingPongSRV);

	// Setup
	pd3dImmediateContext->CSSetConstantBuffers(0, 1, &g_pcbSimulationConstants);
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pGridIndicesUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(3, 1, &g_pGridSRV);

	// Build Grid Indices
	pd3dImmediateContext->CSSetShader(g_pClearGridIndicesCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(NUM_GRID_INDICES / SIMULATION_BLOCK_SIZE, 1, 1);
	pd3dImmediateContext->CSSetShader(g_pBuildGridIndicesCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);

	// Setup
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pSortedParticlesUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(0, 1, &g_pParticlesSRV);
	pd3dImmediateContext->CSSetShaderResources(3, 1, &g_pGridSRV);

	// Rearrange
	pd3dImmediateContext->CSSetShader(g_pRearrangeParticlesCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);

	// Setup
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pNullUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(0, 1, &g_pNullSRV);
	pd3dImmediateContext->CSSetShaderResources(0, 1, &g_pSortedParticlesSRV);
	pd3dImmediateContext->CSSetShaderResources(3, 1, &g_pGridSRV);
	pd3dImmediateContext->CSSetShaderResources(4, 1, &g_pGridIndicesSRV);

	// Density
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pParticleDensityUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShader(g_pDensity_GridCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);

	// Force
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pParticleForcesUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(1, 1, &g_pParticleDensitySRV);
	pd3dImmediateContext->CSSetShader(g_pForce_GridCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);

	// Integrate
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pParticlesUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(2, 1, &g_pParticleForcesSRV);
	pd3dImmediateContext->CSSetShader(g_pIntegrateCS, nullptr, 0);
	pd3dImmediateContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);
}


//--------------------------------------------------------------------------------------
// GPU Fluid Simulation
//--------------------------------------------------------------------------------------
void SimulateFluid(ID3D11DeviceContext* pd3dImmediateContext, f32 fElapsedTime)
{
	u32 UAVInitialCounts = 0;

	// Update per-frame variables
	CBSimulationConstants pData = {};

	// Simulation Constants
	pData.iNumParticles = g_iNumParticles;
	// Clamp the time step when the simulation runs slowly to prevent numerical explosion
	pData.fTimeStep = std::min(g_fMaxAllowableTimeStep, fElapsedTime);
	pData.fSmoothlen = g_fSmoothlen;
	pData.fPressureStiffness = g_fPressureStiffness;
	pData.fRestDensity = g_fRestDensity;
	pData.fDensityCoef = g_fParticleMass * 315.0f / (64.0f * XM_PI * pow(g_fSmoothlen, 9));
	pData.fGradPressureCoef = g_fParticleMass * -45.0f / (XM_PI * pow(g_fSmoothlen, 6));
	pData.fLapViscosityCoef = g_fParticleMass * g_fViscosity * 45.0f / (XM_PI * pow(g_fSmoothlen, 6));

	pData.vGravity = g_vGravity;

	// Cells are spaced the size of the smoothing length search radius
	// That way we only need to search the 8 adjacent cells + current cell
	pData.vGridDim.x = 1.0f / g_fSmoothlen;
	pData.vGridDim.y = 1.0f / g_fSmoothlen;
	pData.vGridDim.z = 0;
	pData.vGridDim.w = 0;

	// Collision information for the map
	pData.fWallStiffness = g_fWallStiffness;
	pData.vPlanes[0] = g_vPlanes[0];
	pData.vPlanes[1] = g_vPlanes[1];
	pData.vPlanes[2] = g_vPlanes[2];
	pData.vPlanes[3] = g_vPlanes[3];
	pData.mousePosition = mousePosition;

	pd3dImmediateContext->UpdateSubresource(g_pcbSimulationConstants, 0, nullptr, &pData, 0, 0);


	if (GetAsyncKeyState('1'))
	{
		g_eSimMode = SIM_MODE_SIMPLE;
	}
	else
		if (GetAsyncKeyState('2'))
		{
			g_eSimMode = SIM_MODE_SHARED;
		}
		else
			if (GetAsyncKeyState('3'))
			{
				g_eSimMode = SIM_MODE_SIMPLE;
			}


	switch (g_eSimMode) {
		// Simple N^2 Algorithm
	case SIM_MODE_SIMPLE:
		SimulateFluid_Simple(pd3dImmediateContext);
		break;

		// Optimized N^2 Algorithm using Shared Memory
	case SIM_MODE_SHARED:
		SimulateFluid_Shared(pd3dImmediateContext);
		break;

		// Optimized Grid + Sort Algorithm
	case SIM_MODE_GRID:
		SimulateFluid_Grid(pd3dImmediateContext);
		break;
	}

	// Unset
	pd3dImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pNullUAV, &UAVInitialCounts);
	pd3dImmediateContext->CSSetShaderResources(0, 1, &g_pNullSRV);
	pd3dImmediateContext->CSSetShaderResources(1, 1, &g_pNullSRV);
	pd3dImmediateContext->CSSetShaderResources(2, 1, &g_pNullSRV);
	pd3dImmediateContext->CSSetShaderResources(3, 1, &g_pNullSRV);
	pd3dImmediateContext->CSSetShaderResources(4, 1, &g_pNullSRV);
}


void Update(ID3D11Device* device, ID3D11DeviceContext* deviceContext, f32 fElapsedTime)
{
	POINT point;
	GetCursorPos((LPPOINT)&point);
	
	std::stringstream ss;
	ss << "x : ";
	ss << point.x;
	ss << "y : ";
	ss << point.y;
	ss << "\n";
	OutputDebugStringA(ss.str().c_str());
	mousePosition.x = point.x;
	mousePosition.y = point.y;
//
//	deviceContext->CSSetShaderResources(0, 1, &m_pRepositionSRVs);
////	deviceContext->CSSetUnorderedAccessViews(0, 1, &m_pRepositionUAVs, &UAVInitialCounts);
//	deviceContext->CSSetConstantBuffers(0, 1, &g_pcbSimulationConstants);
//	deviceContext->CSSetShader(g_pRepositionParticlesCS, nullptr, 0);
//	deviceContext->Dispatch(g_iNumParticles / SIMULATION_BLOCK_SIZE, 1, 1);
//	deviceContext->Flush();
}

//--------------------------------------------------------------------------------------
// GPU Fluid Rendering
//--------------------------------------------------------------------------------------
void RenderFluid(ID3D11Device* pd3dDevice, ID3D11DeviceContext* pd3dImmediateContext, f32 fElapsedTime)
{
	// Simple orthographic projection to display the entire map
	XMMATRIX mView = XMMatrixTranslation(-g_fMapWidth / 2.0f, -g_fMapHeight / 2.0f, 0);
	XMMATRIX mProjection = XMMatrixOrthographicLH(g_fMapWidth, g_fMapHeight, 0, 1);
	XMMATRIX mViewProjection = mView * mProjection;

	// Update Constants
	CBRenderConstants pData = {};

	XMStoreFloat4x4(&pData.mViewProjection, XMMatrixTranspose(mViewProjection));
	pData.fParticleSize = g_fParticleRenderSize;

	pd3dImmediateContext->UpdateSubresource(g_pcbRenderConstants, 0, nullptr, &pData, 0, 0);

	// Set the shaders
	pd3dImmediateContext->VSSetShader(g_pParticleVS, nullptr, 0);
	pd3dImmediateContext->GSSetShader(g_pParticleGS, nullptr, 0);
	pd3dImmediateContext->PSSetShader(g_pParticlePS, nullptr, 0);

	// Set the constant buffers
	pd3dImmediateContext->VSSetConstantBuffers(0, 1, &g_pcbRenderConstants);
	pd3dImmediateContext->GSSetConstantBuffers(0, 1, &g_pcbRenderConstants);
	pd3dImmediateContext->PSSetConstantBuffers(0, 1, &g_pcbRenderConstants);

	// Setup the particles buffer and IA
	pd3dImmediateContext->VSSetShaderResources(0, 1, &g_pParticlesSRV);
	pd3dImmediateContext->VSSetShaderResources(1, 1, &g_pParticleDensitySRV);
	pd3dImmediateContext->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);




	Update(pd3dDevice, pd3dImmediateContext, fElapsedTime);
	// Draw the mesh
	pd3dImmediateContext->Draw(g_iNumParticles, 0);

	// Unset the particles buffer
	pd3dImmediateContext->VSSetShaderResources(0, 1, &g_pNullSRV);
	pd3dImmediateContext->VSSetShaderResources(1, 1, &g_pNullSRV);
}


//--------------------------------------------------------------------------------------
// Render the scene using the D3D11 device
//--------------------------------------------------------------------------------------
void CALLBACK OnD3D11FrameRender(ID3D11Device* pd3dDevice, ID3D11DeviceContext* pd3dImmediateContext, double fTime,
	f32 fElapsedTime, void* pUserContext)
{
	// Clear the render target and depth stencil
	auto pRTV = DXUTGetD3D11RenderTargetView();
	pd3dImmediateContext->ClearRenderTargetView(pRTV, Colors::Black);
	auto pDSV = DXUTGetD3D11DepthStencilView();
	pd3dImmediateContext->ClearDepthStencilView(pDSV, D3D11_CLEAR_DEPTH, 1.0, 5);

	SimulateFluid(pd3dImmediateContext, fElapsedTime);

	RenderFluid(pd3dDevice, pd3dImmediateContext, fElapsedTime);

	RenderText();	
}


//--------------------------------------------------------------------------------------
// Release D3D11 resources created in OnD3D11ResizedSwapChain 
//--------------------------------------------------------------------------------------
void CALLBACK OnD3D11ReleasingSwapChain(void* pUserContext)
{
}


//--------------------------------------------------------------------------------------
// Release D3D11 resources created in OnD3D11CreateDevice 
//--------------------------------------------------------------------------------------
void CALLBACK OnD3D11DestroyDevice(void* pUserContext)
{	
	DXUTGetGlobalResourceCache().OnDestroyDevice();

	SAFE_RELEASE(g_pcbSimulationConstants);
	SAFE_RELEASE(g_pcbRenderConstants);
	SAFE_RELEASE(g_pSortCB);

	SAFE_RELEASE(g_pParticleVS);
	SAFE_RELEASE(g_pParticleGS);
	SAFE_RELEASE(g_pParticlePS);

	SAFE_RELEASE(g_pIntegrateCS);
	SAFE_RELEASE(g_pDensity_SimpleCS);
	SAFE_RELEASE(g_pForce_SimpleCS);
	SAFE_RELEASE(g_pDensity_SharedCS);
	SAFE_RELEASE(g_pForce_SharedCS);
	SAFE_RELEASE(g_pDensity_GridCS);
	SAFE_RELEASE(g_pForce_GridCS);
	SAFE_RELEASE(g_pBuildGridCS);
	SAFE_RELEASE(g_pClearGridIndicesCS);
	SAFE_RELEASE(g_pBuildGridIndicesCS);
	SAFE_RELEASE(g_pRearrangeParticlesCS);
	SAFE_RELEASE(g_pSortBitonic);
	SAFE_RELEASE(g_pSortTranspose);

	SAFE_RELEASE(g_pParticles);
	SAFE_RELEASE(g_pParticlesSRV);
	SAFE_RELEASE(g_pParticlesUAV);

	SAFE_RELEASE(g_pSortedParticles);
	SAFE_RELEASE(g_pSortedParticlesSRV);
	SAFE_RELEASE(g_pSortedParticlesUAV);

	SAFE_RELEASE(g_pParticleForces);
	SAFE_RELEASE(g_pParticleForcesSRV);
	SAFE_RELEASE(g_pParticleForcesUAV);

	SAFE_RELEASE(g_pParticleDensity);
	SAFE_RELEASE(g_pParticleDensitySRV);
	SAFE_RELEASE(g_pParticleDensityUAV);

	SAFE_RELEASE(g_pGridSRV);
	SAFE_RELEASE(g_pGridUAV);
	SAFE_RELEASE(g_pGrid);

	SAFE_RELEASE(g_pGridPingPongSRV);
	SAFE_RELEASE(g_pGridPingPongUAV);
	SAFE_RELEASE(g_pGridPingPong);

	SAFE_RELEASE(g_pGridIndicesSRV);
	SAFE_RELEASE(g_pGridIndicesUAV);
	SAFE_RELEASE(g_pGridIndices);

	//SAFE_RELEASE(g_pRepositionParticlesCS);
}
