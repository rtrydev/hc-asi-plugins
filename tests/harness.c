/* Isolated mechanics test for the HMC d3d8 proxy + widescreen plugin.
 * Mimics what the Contracts renderer does: create a window, Direct3DCreate8, then
 * CreateDevice with EXCLUSIVE-FULLSCREEN parameters (the configuration
 * that fails on CrossOver). If the proxy + plugin work, the plugin rewrites
 * the parameters to windowed and CreateDevice succeeds. Also issues a
 * SetTransform(PROJECTION) to confirm the FOV path runs.
 *
 * Loads the proxy explicitly from the path in argv[1] so we exercise the
 * game-directory copy (its DllMain then scans <dir>/scripts/*.asi). */
#include <d3d8.h>
#include <stdio.h>

static const char *fmtname(D3DFORMAT f){
    switch(f){case D3DFMT_UNKNOWN:return "UNKNOWN";
    case D3DFMT_X8R8G8B8:return "X8R8G8B8";case D3DFMT_A8R8G8B8:return "A8R8G8B8";
    case D3DFMT_R5G6B5:return "R5G6B5";default:return "other";}
}

int main(int argc, char **argv){
    const char *dllpath = argc>1?argv[1]:"d3d8.dll";
    HMODULE h = LoadLibraryA(dllpath);
    if(!h){ printf("FAIL: LoadLibrary(%s) err=%lu\n", dllpath, GetLastError()); return 2; }
    printf("proxy loaded at %p\n", (void*)h);

    typedef IDirect3D8* (WINAPI *dc_t)(UINT);
    dc_t Create = (dc_t)(void*)GetProcAddress(h, "Direct3DCreate8");
    if(!Create){ printf("FAIL: no Direct3DCreate8 export\n"); return 2; }

    IDirect3D8 *d3d = Create(D3D_SDK_VERSION);
    if(!d3d){ printf("FAIL: Direct3DCreate8 returned NULL\n"); return 2; }
    printf("IDirect3D8 = %p\n", (void*)d3d);

    D3DDISPLAYMODE dm; d3d->lpVtbl->GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &dm);
    printf("desktop mode %ux%u fmt=%s\n", dm.Width, dm.Height, fmtname(dm.Format));

    WNDCLASSA wc; ZeroMemory(&wc,sizeof(wc));
    wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(NULL);
    wc.lpszClassName="HMCTestWnd"; RegisterClassA(&wc);
    HWND wnd=CreateWindowExA(0,"HMCTestWnd","h2sa",WS_POPUP,0,0,800,600,
        NULL,NULL,wc.hInstance,NULL);
    printf("window = %p\n", (void*)wnd);

    /* exclusive fullscreen request, like the stock game */
    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp,sizeof(pp));
    pp.BackBufferWidth=800; pp.BackBufferHeight=600;
    pp.BackBufferFormat=D3DFMT_X8R8G8B8; pp.BackBufferCount=1;
    pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=wnd;
    pp.Windowed=FALSE; pp.EnableAutoDepthStencil=TRUE;
    pp.AutoDepthStencilFormat=D3DFMT_D16;
    pp.FullScreen_RefreshRateInHz=D3DPRESENT_RATE_DEFAULT;

    IDirect3DDevice8 *dev=NULL;
    HRESULT hr=d3d->lpVtbl->CreateDevice(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,
        wnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev);
    printf("CreateDevice hr=0x%08lx dev=%p  (after fixup: %ux%u windowed=%d fmt=%s)\n",
        (unsigned long)hr,(void*)dev,pp.BackBufferWidth,pp.BackBufferHeight,
        pp.Windowed,fmtname(pp.BackBufferFormat));
    if(FAILED(hr)||!dev){ printf("RESULT: CreateDevice FAILED\n"); return 1; }

    /* perspective projection: _34=1,_44=0. Feed a 4:3-authored matrix and
     * check the plugin scales _11 for the (wide) backbuffer aspect. */
    D3DMATRIX m; ZeroMemory(&m,sizeof(m));
    float w=1.3f, hh=1.7f;              /* arbitrary xScale/yScale */
    m._11=w; m._22=hh; m._33=1.0f; m._34=1.0f;
    printf("proj _11 before = %.5f\n", m._11);
    dev->lpVtbl->SetTransform(dev,D3DTS_PROJECTION,&m);
    /* Read back what the device stored to see the applied value. */
    D3DMATRIX got; dev->lpVtbl->GetTransform(dev,D3DTS_PROJECTION,&got);
    printf("proj _11 stored  = %.5f (bb %ux%u aspect %.3f)\n",
        got._11, pp.BackBufferWidth, pp.BackBufferHeight,
        (double)pp.BackBufferWidth/pp.BackBufferHeight);

    /* UIScale's fast VB path transforms data inside the engine's existing
     * write lock. Verify the write -> Unlock -> direct-buffer result without
     * relying on the game renderer. */
    typedef void (WINAPI *setuiscale_t)(float, float);
    setuiscale_t setuiscale = (setuiscale_t)(void*)
        GetProcAddress(h, "HMC_SetUIScale");
    IDirect3DVertexBuffer8 *vb = NULL;
    const DWORD ui_fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
    struct UIVertex { float x,y,z,rhw; DWORD color; float u,v; };
    hr = dev->lpVtbl->CreateVertexBuffer(dev, 4*sizeof(struct UIVertex),
        D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY, ui_fvf, D3DPOOL_DEFAULT, &vb);
    if(!setuiscale || FAILED(hr) || !vb){
        printf("RESULT: UIScale VB setup FAILED\n"); return 1;
    }
    setuiscale(2.0f, 2.0f);
    dev->lpVtbl->SetVertexShader(dev, ui_fvf);
    BYTE *mapped = NULL;
    hr = vb->lpVtbl->Lock(vb, 0, 0, &mapped, D3DLOCK_DISCARD);
    if(SUCCEEDED(hr) && mapped){
        struct UIVertex *v = (struct UIVertex*)mapped;
        for(int i=0;i<4;i++){
            v[i].x=10.0f+i; v[i].y=20.0f+i; v[i].z=0.0f; v[i].rhw=1.0f;
            v[i].color=0xffffffff; v[i].u=0.0f; v[i].v=0.0f;
        }
        vb->lpVtbl->Unlock(vb);
        mapped = NULL;
        hr = vb->lpVtbl->Lock(vb, 0, sizeof(struct UIVertex), &mapped,
                              D3DLOCK_READONLY);
    }
    float scaled_x = mapped ? ((struct UIVertex*)mapped)->x : -1.0f;
    if(mapped) vb->lpVtbl->Unlock(vb);
    vb->lpVtbl->Release(vb);
    setuiscale(1.0f, 1.0f);
    printf("UIScale VB x 10.0 -> %.1f (expected 20.5)\n", scaled_x);
    if(scaled_x < 20.49f || scaled_x > 20.51f){
        printf("RESULT: UIScale VB transform FAILED\n"); return 1;
    }

    /* In-game resolution switch, part 1: Reset to a new backbuffer size (what
     * the engine does when a new mode is picked in the video options). On
     * native D3D8 this is where two historical bugs met: a non-DEFAULT
     * windowed presentation interval fails the Reset with INVALIDCALL, and a
     * loader-held backbuffer reference fails it as an outstanding swapchain
     * ref. */
    D3DPRESENT_PARAMETERS pp2 = pp;
    pp2.BackBufferWidth = 1280; pp2.BackBufferHeight = 720;
    hr = dev->lpVtbl->Reset(dev, &pp2);
    printf("Reset(1280x720) hr=0x%08lx (after fixup: %ux%u windowed=%d "
           "interval=0x%lx)\n", (unsigned long)hr,
           pp2.BackBufferWidth, pp2.BackBufferHeight, pp2.Windowed,
           (unsigned long)pp2.FullScreen_PresentationInterval);
    int reset_ok = SUCCEEDED(hr);

    /* Part 2: full device teardown + recreation (the engine's fallback when
     * Reset fails, and a settings-change path in its own right). With the
     * old loader this Released a dangling backbuffer pointer inside
     * capture_backbuffer and crashed (the native-Windows resolution-switch
     * crash); it must survive and create cleanly. */
    dev->lpVtbl->Release(dev); dev = NULL;
    D3DPRESENT_PARAMETERS pp3; ZeroMemory(&pp3, sizeof(pp3));
    pp3.BackBufferWidth = 1920; pp3.BackBufferHeight = 1080;
    pp3.BackBufferFormat = D3DFMT_X8R8G8B8; pp3.BackBufferCount = 1;
    pp3.SwapEffect = D3DSWAPEFFECT_DISCARD; pp3.hDeviceWindow = wnd;
    pp3.Windowed = FALSE; pp3.EnableAutoDepthStencil = TRUE;
    pp3.AutoDepthStencilFormat = D3DFMT_D16;
    pp3.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;
    hr = d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
        wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp3, &dev);
    printf("re-CreateDevice hr=0x%08lx dev=%p (after fixup: %ux%u windowed=%d)\n",
        (unsigned long)hr, (void*)dev,
        pp3.BackBufferWidth, pp3.BackBufferHeight, pp3.Windowed);
    if(FAILED(hr)||!dev){ printf("RESULT: re-CreateDevice FAILED\n"); return 1; }
    if(!reset_ok){ printf("RESULT: Reset FAILED\n"); return 1; }

    printf("RESULT: OK\n");
    dev->lpVtbl->Release(dev);
    d3d->lpVtbl->Release(d3d);
    return 0;
}
