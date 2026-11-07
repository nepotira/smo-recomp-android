# Native Aspect Ratio (Hor+) Patches for Super Mario Odyssey

This directory contains instruction-level AOT patches applied by `lifter/recomp.py` through the `RECOMP_PATCHES` environment variable.

---

## 1. How It Works

1. **The Problem:**  
   The Nintendo Switch natively outputs a 16:9 aspect ratio ($1.7777778\text{f}$). Modern flagship Android smartphones (such as the Xiaomi Poco F7 with a $2712 \times 1220$ resolution) have elongated screens of approximately 20:9 ($2.2229508\text{f}$). If stretched, geometries suffer a $25.04\%$ horizontal distortion. If letterboxed, large black bars waste screen real estate.

2. **The Hor+ Solution:**  
   As proven in the descompiled engine source code ([`odyssey-decomp/lib/al/Library/Scene/SceneUtil.cpp`](file:///home/nepo/Downloads/SWITCH/pesquisa_fontes/odyssey-decomp/lib/al/Library/Scene/SceneUtil.cpp), line 140), `al::getSceneFrameBufferMainAspect` calculates the aspect ratio used to configure the 3D perspective projection matrix in `al::Projection::setAspect` ([`Projection.cpp`](file:///home/nepo/Downloads/SWITCH/pesquisa_fontes/odyssey-decomp/lib/al/Library/Projection/Projection.cpp), line 34):
   ```cpp
   void Projection::setAspect(f32 aspect) {
       mBase.setAspect(aspect);
       mAspect = aspect;
   }
   f32 Projection::calcNearClipWidth() {
       return calcNearClipHeight() * mBase.getAspect();
   }
   ```
   By supplying the native aspect ratio ($2.2229508\text{f}$), `calcNearClipWidth` widens the camera frustum by $+25.04\%$ while keeping vertical height ($1.0\times$) unchanged. Mario, Cappy, and all circular objects retain perfect proportions with extra world geometry rendered on the sides.

3. **2D Safe-Zone Isolation (`patches/smo_safe_zone_2d.txt`):**  
   Orthographic 2D UI render passes (life counter, coins, moons, action commands) remain anchored in a centered 16:9 safe zone ($2169 \times 1220$ on Poco F7 with $271.5\text{px}$ side margins) to prevent horizontal stretching and obstruction by the front camera punch-hole. Touch controls in `runtime/android/nnpad_android.c` automatically apply inward margin compensation on displays wider than 16:9.

---

## 2. Usage

To apply the patches during the AOT transpilation step:
```bash
export RECOMP_PATCHES="patches/smo_aspect_ratio_20_9.txt patches/smo_safe_zone_2d.txt"
python lifter/recomp.py full games/smo/generated
```
