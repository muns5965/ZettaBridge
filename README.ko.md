# ZettaBridge — Android 17용 StyleTap 포팅

**한국어** · [English](README.md) · [기존 ZettaBridge README](docs/README-original.md)

> **실험 중인 개발 브랜치입니다. 아직 StyleTap이 정상 실행되지 않습니다.** 공식 StyleTap 배포판이 아니라 호환성 포팅 작업입니다.

## 목표

기존 **StyleTap 0.8.0034** Android 앱의 32비트 `armeabi-v7a` 라이브러리를 ZettaBridge의 ARM32 게스트 런타임을 통해 **ARM64 전용 Android 17** 기기에서 실행하는 것이 목표입니다. 루팅 없이 동작하도록 개발하고 있습니다.

- 저장소: `muns5965/ZettaBridge`
- 작업 브랜치: `feature/styletap-android17`
- 원본 프로젝트: ZettaBridge(기존 설명은 보관 문서 참조)
- 실기기 검증 전에는 개발 내용을 `main`에 병합하지 않습니다.

## 현재까지 확인된 사항

| 단계 | 상태 |
| --- | --- |
| 디버그 런처 APK 빌드 및 설치 | 확인 |
| StyleTap APK 가져오기 | 확인 |
| ARM32 게스트 링커 실행 | Dynarmic 패치 적용 후 확인 |
| `libStyleTap.so` 로딩 | 확인 (`proxy-loads: 1`, `proxy-failures: 0`) |
| JNI 네이티브 메서드 등록 | 확인 (18개) |
| StyleTap Activity 및 화면 시작 | **충돌 발생 — 해결 미검증** |
| Palm OS 화면·입력·앱 실행 | **미검증** |

### 현재 해결 중인 오류

Android 17에서 마지막으로 확인된 예외는 다음과 같습니다.

```text
java.lang.NullPointerException:
  DummyEditText.addTextChangedListener(...) on a null object reference
at com.StyleTap.StyleTap.StyleTap.onCreate(StyleTap.java:2994)
```

플러그인 Activity의 Window에 호스트 리소스를 사용하는 레이아웃 인플레이터가 남아 있을 가능성을 확인했습니다. 커밋 `f074136b`에서 이를 플러그인 Activity 기준으로 다시 연결했지만, **실제 기기에서 아직 수정 효과를 확인하지 못했습니다.** 이전 ARM32 링커의 `SIGILL`은 후속 런타임 보고서에서 발생하지 않았습니다.

## 빌드 및 테스트

1. [StyleTap 디버그 APK Actions](https://github.com/muns5965/ZettaBridge/actions/workflows/styletap-debug-apk.yml)로 이동합니다.
2. **`feature/styletap-android17` 브랜치**의 최신 수정사항이 포함된 성공 빌드를 골라 APK 아티팩트를 내려받습니다.
3. ZettaBridge를 설치하고 **합법적으로 확보한** StyleTap APK를 가져온 뒤 실행합니다.
4. 충돌하면 ZettaBridge를 다시 열고 StyleTap 항목을 길게 누른 다음 **Last run report**를 선택합니다. ARM32 런타임 정보와 Java 예외, 가능한 경우 Android 프로세스 종료 이력이 함께 출력되며, 복사해서 전달할 수 있습니다. **ADB는 필요하지 않습니다.**

CI 디버그 APK의 서명이 빌드마다 달라 업데이트가 거부될 수 있습니다. 기존 데이터를 보존하기 전에 앱을 삭제하지 마세요.

## 구현 및 주의사항

- Dynarmic 패치는 `third_party/patches/`에 있으며 CI에서 필수로 적용합니다.
- Android Bitmap 호스트 브리지가 실험적으로 추가되었으나 화면 출력은 아직 검증되지 않았습니다.
- 런처 소스 수정 시 개발 브랜치의 디버그 APK 워크플로가 자동 실행되도록 설정했습니다.
- 저작권이 있는 StyleTap 원본 APK 및 네이티브 라이브러리는 **저장소에 커밋하지 않습니다.**

## 이전 문서

기존 ZettaBridge 설명은 변경 없이 아래 위치에 보관했습니다.
- [기존 영문 README](docs/README-original.md)
- [기존 러시아어 README](docs/README-original.ru.md)

루트 README에는 **현재 StyleTap 포팅 작업에 관한 내용만** 남깁니다.
