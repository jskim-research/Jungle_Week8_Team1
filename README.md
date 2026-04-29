# NipsEngine - 8주차 제출 README

## 프로젝트 개요

이번 제출물은 커스텀 DirectX 11 엔진에서 구현된 그림자 렌더링 기능을 확인하기 위한 실행형 데모입니다.  
평가자는 소스 코드 없이 실행 파일과 런타임 폴더만으로 아래 항목을 직접 확인할 수 있습니다.

- Directional Light, Point Light, Spot Light의 그림자 생성
- `Projection Mode` 전환: `Standard`, `PSM`
- `Filter Mode` 전환: `SSM`, `SSM + PCF`, `VSM`
- 광원별 `Cast Shadows`, `Shadow Resolution Scale`, `Shadow Bias`, `Shadow Slope Bias`, `Shadow Sharpen`
- `Shadow Map Preview`를 통한 그림자 맵 확인
- `Override Camera`를 통한 광원 시점 확인
- `Shadow Stats` 창에서 광원 수, Shadow Map 수, 해상도, 포맷, 메모리 확인
- `Directional Light Debug`, `Point Light Debug`, `Spot Light Debug` 표시

## 실행 방법

- 제출 폴더에서 `NipsEngine.exe`를 실행합니다.
- 실행 파일과 같은 위치에 `Asset/`, `Shaders/`, `Settings/` 폴더가 있어야 합니다.
- `imgui.ini`가 함께 제공되면 기본 UI 배치를 바로 재현할 수 있습니다.
- 패널이 닫혀 있으면 상단 `View` 메뉴에서 `Scene Manager`, `Property`, `Viewport Settings`를 다시 열 수 있습니다.

## 가장 먼저 보면 좋은 것

1. `NipsEngine.exe`를 실행합니다.
2. 상단 `Files > Load Scene`에서 `PSM_Test.Scene`을 불러옵니다.
3. `Scene Manager` 또는 뷰포트에서 Directional Light 또는 Spot Light를 선택합니다.
4. `Property` 창에서 `Cast Shadows`, `Apply PSM`, `Shadow Bias`, `Shadow Slope Bias`, `Shadow Resolution Scale`을 확인합니다.
5. `Viewport Settings > Shadow Settings`에서 `Projection Mode`와 `Filter Mode`를 바꿔가며 결과를 비교합니다.
6. 같은 `Property` 창의 `Shadow Map Preview`와 `Override Camera`로 그림자 맵과 광원 시점을 확인합니다.
7. 뷰포트 상단 오버레이 메뉴의 `Stats > Shadow`를 켜서 `Shadow Stats` 창을 확인합니다.

## 추천 확인 씬

### `PSM_Test.Scene`

- `Standard`와 `PSM` 비교에 가장 적합한 씬입니다.
- Directional Light와 Spot Light가 모두 배치되어 있어 투영 방식 차이를 보기 쉽습니다.
- `Shadow Map Preview`, `Override Camera`, bias 조절, 필터 전환을 먼저 확인하기 좋습니다.

### `Multiple_Light.Scene`

- 여러 Point Light, Spot Light, Directional Light가 동시에 그림자를 생성하는 장면입니다.
- 다수 광원 환경에서 shadow가 안정적으로 유지되는지 확인하기 좋습니다.
- `Shadow Stats`에서 광원 수, Shadow Map 수, 해상도, 메모리 요약을 보기 좋습니다.
- Point Light 선택 시 `Shadow Map Preview`의 `Face` 전환으로 6방향 그림자 확인이 가능합니다.

### `Scene_01_LightComponents_whitewall.Scene`

- 밝은 배경과 단순한 배치 덕분에 그림자 경계와 아티팩트를 보기 쉬운 씬입니다.
- `Cast Shadows` on/off, `Shadow Bias`, `Shadow Slope Bias`, `Shadow Resolution Scale` 변화 확인에 적합합니다.
- Shadow acne, peter panning, 해상도 차이에 따른 계단 현상 관찰용으로 추천합니다.

## 핵심 기능별 확인 방법

### 1. Shadow Mapping

- 광원과 수광면 사이에 오브젝트가 있으면 그림자가 생성됩니다.
- 광원의 위치 또는 방향을 바꾸면 그림자 방향과 길이가 함께 변합니다.
- 그림자를 만드는 오브젝트를 움직이면 그림자도 즉시 따라 움직입니다.

### 2. 광원 시점과 Shadow Map 확인

- 그림자를 만드는 광원을 선택하면 `Property` 창에 `Shadow Map Preview`가 표시됩니다.
- `Override Camera`를 누르면 선택한 광원 기준으로 장면을 확인할 수 있습니다.
- Directional Light는 `Cascade` 단위로 확인할 수 있습니다.
- Point Light는 `Face` 선택으로 `+X`, `-X`, `+Y`, `-Y`, `+Z`, `-Z` 방향을 확인할 수 있습니다.
- Spot Light는 `Shadow Atlas` 슬롯 기준으로 preview를 확인할 수 있습니다.

### 3. Cast Shadows 토글

- 현재 제출본에서 명확히 확인 가능한 on/off 옵션은 광원별 `Cast Shadows`입니다.
- 동일한 씬에서 이 값을 껐다 켜면 해당 광원의 그림자 생성 여부를 즉시 비교할 수 있습니다.

### 4. Directional Light Shadow

- `PSM_Test.Scene` 또는 `Scene_01_LightComponents_whitewall.Scene`에서 확인을 권장합니다.
- `Property` 창에서 `Cascade Count`, `Cascade Splits`, `Shadow Distance`, `Apply PSM`을 확인할 수 있습니다.
- `Shadow Map Preview`에서 cascade별 그림자 맵을 비교할 수 있습니다.
- 광원 회전에 따라 장면 전체 그림자 방향이 바뀌는지 확인하면 됩니다.

### 5. Spot Light Shadow

- `PSM_Test.Scene`과 `Multiple_Light.Scene`에서 확인하기 좋습니다.
- `Inner Cone Angle`, `Outer Cone Angle`, `Cast Shadows`, `Apply PSM`을 직접 조절할 수 있습니다.
- Spot Light를 회전시키면 원뿔 범위에 맞춰 그림자가 변하는지 확인할 수 있습니다.
- `Shadow Map Preview`에서 atlas 기반 preview를 확인할 수 있습니다.

### 6. Point Light Shadow

- `Multiple_Light.Scene`에서 확인을 권장합니다.
- Point Light 선택 시 `Shadow Map Preview`의 `Face` 전환으로 6방향 그림자를 확인할 수 있습니다.
- Point Light의 위치를 바꾸면 주변 모든 방향의 그림자 결과가 함께 바뀌는지 확인하면 됩니다.

### 7. PSM

- `Viewport Settings > Shadow Settings > Projection Mode`에서 `Standard`와 `PSM`을 전환할 수 있습니다.
- `PSM_Test.Scene`에서 비교하는 것이 가장 쉽습니다.
- Directional Light와 Spot Light의 `Property`에 `Apply PSM` 항목이 있어, 투영 방식 비교가 가능합니다.
- 같은 장면과 비슷한 해상도에서 카메라에 보이는 영역의 그림자 분포 차이를 비교해 보면 됩니다.

### 8. Shadow Filtering

- `Viewport Settings > Shadow Settings > Filter Mode`에서 `SSM`, `SSM + PCF`, `VSM`을 선택할 수 있습니다.
- `SSM`은 비교적 단단한 경계를 확인하기 좋습니다.
- `SSM + PCF`는 가장자리 비교가 더 부드럽게 보이는지 확인하면 됩니다.
- `VSM`은 Directional Light와 Point Light에서 차이가 비교적 잘 드러납니다.
- Spot Light는 필터 비교 시에도 depth-atlas 기반 결과를 중심으로 확인하면 됩니다.

### 9. Bias와 아티팩트 확인

- 광원 `Property`에서 `Shadow Bias`, `Shadow Slope Bias`, `Shadow Sharpen`, `Shadow Resolution Scale`을 조절할 수 있습니다.
- `Scene_01_LightComponents_whitewall.Scene`에서 오브젝트 접점과 바닥 그림자를 보면 변화가 잘 보입니다.
- Bias가 너무 작으면 자기 자신을 잘못 가리는 shadow acne가 나타날 수 있습니다.
- Bias가 너무 크면 그림자가 물체에서 떨어져 보이는 peter panning이 나타날 수 있습니다.
- `Shadow Sharpen`은 경계 인상을 비교할 때 함께 보기 좋습니다.

### 10. Shadow Resolution / Multi-light / Stats

- 광원별 `Shadow Resolution Scale`을 바꾸면 그림자 선명도와 계단 현상 차이를 확인할 수 있습니다.
- 뷰포트 상단 오버레이 메뉴의 `Stats > Shadow`를 켜면 `Shadow Stats` 창이 열립니다.
- 상단 요약에는 shadow-casting light 수, logical map 수, resource view 수, 총 메모리 요약이 표시됩니다.
- 표에는 `Light`, `Type`, `Cast Shadow`, `Projection`, `Filter`, `Shadow Maps`, `Resolution`, `Format`, `Memory`가 표시됩니다.
- `Multiple_Light.Scene`에서 여러 광원이 동시에 그림자를 사용할 때 map 수와 메모리 변화가 잘 드러납니다.

### 11. 디버그 표시와 보조 뷰

- `Viewport Settings > Light Settings`에서 `Directional Light Debug`, `Point Light Debug`, `Spot Light Debug`를 켤 수 있습니다.
- 각 뷰포트 상단 메뉴의 `View`에서 `Lit`, `Unlit`, `Wireframe`, `Scene Depth`, `World Normal`을 선택할 수 있습니다.
- 그림자 확인 자체는 `Lit`이 가장 직접적이고, 보조 분석은 `Scene Depth`와 `World Normal`이 유용합니다.

## 사용 방법

- 씬 열기: `Files > Load Scene` 또는 `Ctrl+O`
- 씬 저장: `Files > Save Scene` 또는 `Ctrl+S`
- 패널 다시 열기: 상단 `View` 메뉴
- 뷰포트 레이아웃: 각 뷰포트 상단의 `Layout > SingleView / Quad View`
- 뷰포트 타입: `Type` 메뉴에서 `Perspective`, `Top`, `Bottom`, `Front`, `Back`, `Left`, `Right`
- 카메라 회전: `Mouse Right Drag`
- 카메라 팬 이동: `Mouse Middle Drag`
- 돌리 인/아웃: `Alt + Mouse Right Drag`
- FOV 또는 직교 높이 조절: `Mouse Wheel`
- 카메라 이동: `W / A / S / D / Q / E` (회전 중)
- 선택 포커스: `F`
- 단일 선택: `Mouse Left Click`
- 선택 추가/토글: `Shift + Click`, `Ctrl + Click`
- 박스 선택: `Ctrl + Alt + Drag`
- 기즈모 모드 순환: `Space`
- 월드/로컬 전환: `X`
- 선택 삭제: `Delete`

## 평가자가 보면 좋은 체크 포인트

- 오브젝트 뒤쪽에 광원 방향에 맞는 그림자가 생성되는가
- 광원 위치나 방향을 바꾸면 그림자 방향과 길이가 함께 바뀌는가
- 오브젝트를 이동하면 투영된 그림자가 즉시 따라 움직이는가
- `Cast Shadows`를 끄면 해당 광원의 그림자가 사라지는가
- `Standard`와 `PSM` 전환 시 동일 장면에서 그림자 분포 차이가 보이는가
- `SSM`, `SSM + PCF`, `VSM` 전환 시 경계 표현 차이가 보이는가
- `Shadow Bias`를 높이거나 낮출 때 acne 또는 peter panning 변화가 보이는가
- `Shadow Resolution Scale` 증가 시 그림자가 더 선명해지고 `Shadow Stats`의 해상도/메모리 수치가 달라지는가
- Directional Light 선택 시 cascade 단위 preview가 가능한가
- Point Light 선택 시 `Face` 전환으로 6방향 preview가 가능한가
- 여러 광원이 동시에 그림자를 사용하는 장면에서도 렌더링이 유지되는가

## 핵심 키워드

- Shadow Mapping
- Light View
- Light Projection
- Depth Buffer
- Shadow Map
- Cast Shadows
- Cascade
- Cube Face
- Shadow Atlas
- PSM
- SSM
- PCF
- VSM
- Shadow Bias
- Shadow Slope Bias
- Shadow Sharpen
- Shadow Resolution Scale
- Shadow Acne
- Peter Panning
- Multi-light Shadow

## 요약

이번 제출물은 그림자 렌더링 자체를 실행 파일 기준으로 검증할 수 있도록 구성되어 있으며, Directional / Point / Spot Light 그림자, PSM 전환, 필터 비교, bias 조절, shadow map preview, 그리고 shadow 통계 창까지 한 번에 확인할 수 있는 Shadow 중심 데모입니다.
