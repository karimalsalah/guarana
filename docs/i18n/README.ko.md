**언어:** [English](../../README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português (Brasil)](README.pt-BR.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [목록](README.md)

# Guarana

**내장 GPU에 맞춰 조정한 Nintendo Switch 2 에뮬레이터.**

Guarana는 [Lichii](https://github.com/zortos293/Lichii)(`nx2` 브랜치)에서 갈라져 나온 오픈소스 프로젝트이며 Windows x64용입니다. 내장 그래픽이 있는 노트북과 휴대용 기기를 대상으로 하며, 먼저 Intel Arc를, 다음으로 AMD Radeon을 지원할 예정입니다. 프레임 페이싱 안정화, 프레임 생성, 빠른 부팅, 전력 예산 배분 개선에 중점을 둡니다.

> [!IMPORTANT]
> Guarana에는 키, 펌웨어, 게임, 시스템 파일이 **포함되어 있지 않습니다**. 직접 소유한 콘솔과 게임에서 추출한 파일을 준비해야 합니다. Nintendo 및 Lichii 프로젝트와 제휴 관계가 없습니다. 아래 법적 안내를 참고하세요.

## 소개

- GPL-3.0-or-later 라이선스로 배포되는 무료 오픈소스 에뮬레이터입니다.
- 공유 메모리와 전력이 제한된 내장 GPU에 맞춰 조정되어 있습니다.
- Windows x64, AVX2를 지원하는 CPU, 최신 그래픽 드라이버가 필요합니다.
- 키, 펌웨어, 게임, 시스템 파일은 포함되지 않습니다.

## 측정 결과

노트북 한 대(Core Ultra 9 285H, Arc 140T 내장 GPU, Intel 드라이버 32.0.101.9175, 메모리 32 GB)에서 측정했습니다. *Ocarina of Time*의 Hyrule Field에서 전역 메모리 라우팅 수정(`EDEN_NX2_GM=11`)을 켜자 게임이 직접 렌더링하는 프레임 속도가 **14.8 fps**에서 **22.8 fps**로 올라갔습니다. 같은 위치에서 켜기와 끄기를 비교했으며, 프레임 생성은 꺼 둔 상태였습니다.

이것은 한 기기, 한 게임, 한 장면의 결과일 뿐입니다. 다른 기기, 드라이버, 게임, 장면은 아직 측정하지 않았습니다. [측정에 참여](../../docs/BENCHMARKING.md)해 주세요.

## 지원 현황

지금까지 테스트한 것은 Arc 140T뿐입니다. 그 밖의 GPU(Arc 140V, Arc B390, Radeon 780M, 880M, 890M 등)는 아직 테스트하지 않았습니다. [기기 보고서](../../docs/DEVICES.md#send-a-device-report)를 보내 주시면 도움이 됩니다. 공유 메모리가 16 GB인 기기는 권장 최소 사양보다 낮으니 [DEVICES.md](../../docs/DEVICES.md)를 확인하세요.

## 빠른 시작

1. [Releases](https://github.com/karimalsalah/guarana/releases)에서 최신 zip 파일을 받습니다. 공식 다운로드 위치는 이 페이지뿐입니다.
2. zip 전체를 쓰기 권한이 있는 폴더에 풀고 **`lichii.exe`**를 실행합니다.
3. 키와 펌웨어를 설치합니다. **Tools → Install Decryption Keys**, 그다음 **Tools → Install Firmware** 순서로 실행하세요.
4. 게임 폴더를 추가하고(빈 게임 목록을 두 번 클릭) 게임을 시작합니다. 각 지역에 처음 들어갈 때는 셰이더를 컴파일하느라 끊김이 생길 수 있으며, 이후에는 캐시에서 불러옵니다.
5. 선택 사항: **Emulation → Configure… → Graphics**에서 **Enable Frame Generation**을 켭니다.

설정, 세이브, 캐시는 `%APPDATA%\lichii`에 저장되며, 이는 상위 프로젝트 Lichii와 같은 폴더입니다. 두 버전을 바꿔 쓰기 전에 백업하세요. 모든 스위치는 [docs/switches.md](../../docs/switches.md)에, 자주 생기는 문제는 [TROUBLESHOOTING.md](../../docs/TROUBLESHOOTING.md)에 있습니다.

## 키, 펌웨어, 게임 (법적 안내)

- 직접 소유한 하드웨어에서 추출한 키, 펌웨어, 게임을 사용하세요. Guarana는 이것들을 제공하지 않으며, issue, PR, 토론에서 공유하는 것도 받지 않습니다.
- 저작권이 있는 파일을 요청하거나 링크를 공유하는 내용, 그런 파일을 얻는 방법에 관한 질문은 처리하지 않습니다.
- Guarana는 독립 프로젝트입니다. Nintendo 또는 Lichii 프로젝트의 승인이나 후원을 받지 않았습니다. "Nintendo"와 "Nintendo Switch"는 Nintendo의 상표이며, 이 소프트웨어와 호환되는 대상을 설명하는 목적으로만 사용했습니다. 전문은 [LEGAL.md](../../LEGAL.md)를 보세요.

## 기여하기

- **코드 없이 참여:** [벤치마크](../../docs/BENCHMARKING.md)를 실행해 결과를 보내거나 [기기 보고서](../../docs/DEVICES.md#send-a-device-report)를 제출하세요.
- **코드로 참여:** `good first issue` 라벨이 붙은 issue부터 시작하고, [CONTRIBUTING.md](../../CONTRIBUTING.md)와 [빌드 안내](../../docs/Build.md)를 읽으세요.
- **AI 에이전트:** 먼저 [AGENTS.md](../../AGENTS.md), [아키텍처 문서](../../docs/ARCHITECTURE.md), [AI 정책](../../docs/AI_POLICY.md)을 읽으세요. 모든 PR에는 책임지는 사람이 있어야 합니다.

## 링크

- [영문 README(기준 문서)](../../README.md) · [웹사이트](https://karimalsalah.github.io/guarana/) · [FAQ](../../docs/FAQ.md)
- [로드맵](../../ROADMAP.md) · [변경 기록](../../CHANGELOG.md) · [라이선스](../../LICENSE.txt)

## 라이선스

Guarana는 **GNU General Public License v3.0 이상**에 따라 배포되는 자유 소프트웨어입니다.

이 번역은 영문판보다 늦을 수 있습니다. 내용이 다르면 영문 README가 우선합니다. 이 번역은 아직 원어민의 검토를 받지 않았습니다.
