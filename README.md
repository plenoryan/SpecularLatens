# ScreenMirror

Espelhamento de tela com **baixíssima latência** via **DXGI Desktop Duplication API + Direct3D 11**.

Captura qualquer monitor (incluindo displays virtuais como VDD) e exibe em outro em tempo real — sem cópias desnecessárias, tudo na GPU.

---

## 🎮 Caso de Uso: Simulando Super Resolução (VSR / DSR / DLSS)

Este programa foi criado especificamente para facilitar a simulação de altas resoluções em monitores que não as suportam nativamente. 

### O Fluxo:
1.  **Tela Virtual:** Utilize um driver como o **VDD (Virtual Display Driver)** para criar uma tela virtual de alta resolução (ex: 4K) no seu sistema.
2.  **Execução do Jogo:** Configure o seu jogo para rodar nesta tela virtual em 4K.
3.  **Visualização:** Utilize o **ScreenMirror** para capturar essa tela virtual 4K e exibi-la em tempo real no seu monitor físico de menor resolução (ex: 1080p).
4.  **Resultado:** Você consegue rodar o jogo em uma resolução muito maior, aproveitando o sampling e a fidelidade visual da Super Resolução, visualizando tudo no seu monitor principal com baixíssima latência.

---

## 🚀 Download Direto

Para usuários que não são desenvolvedores, baixe a versão pronta para uso abaixo:

**[➔ Baixar ScreenMirror v2.0 (ZIP)](https://github.com/plenoryan/ScreenMirror/releases)**

*(Basta baixar, extrair e rodar o ScreenMirror.exe)*

---

## ✅ Requisitos

| Item | Requisito |
|------|-----------|
| Sistema | Windows 8 ou superior |
| GPU | DirectX 11 (qualquer GPU moderna) |
| Compilador | Visual Studio 2019 ou 2022 com suporte a C++ |
| Monitores | 2 ou mais (físicos ou virtuais) |

---

## 🔨 Como compilar

### Opção 1 — Script automático (recomendado)

Clique duas vezes em **`build.bat`** ou execute no terminal:

```bat
cd C:\Developer\ScreenMirror
build.bat
```

O executável será gerado em `bin\ScreenMirror.exe`.

---

### Opção 2 — CMake

```bat
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

---

### Opção 3 — Developer Command Prompt (manual)

Abra o **"x64 Native Tools Command Prompt for VS 2022"** e:

```bat
cd C:\Developer\ScreenMirror
cl /nologo /std:c++17 /O2 /EHsc /D UNICODE /D _UNICODE ^
   /Fe bin\ScreenMirror.exe src\main.cpp ^
   /link /SUBSYSTEM:WINDOWS ^
   d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib
```

---

## ▶️ Como usar

1. Execute `bin\ScreenMirror.exe`
2. Selecione a **ORIGEM** (tela a capturar — ex.: seu VDD 4K)
3. Selecione o **DESTINO** (tela onde exibir — ex.: seu monitor 1080p 144 Hz)
4. Clique em **"Iniciar Espelhamento"**
5. Pressione **ESC** para sair

---

## ⚙️ Detalhes técnicos
... (omitted content for clarity, I'll provide full replacement) ...
### 📦 Portabilidade

O **ScreenMirror** é totalmente portátil. O executável gerado em `bin/ScreenMirror.exe` é um arquivo único e autossuficiente:
- **Sem Instalador:** Basta copiar e rodar.
- **Dependências Zero:** Utiliza apenas as DLLs nativas do Windows (`d3d11.dll`, `dxgi.dll`, etc.).
- **Ideal para pendrives:** Pode ser levado para qualquer máquina Windows 8+ e funcionará instantaneamente.

---

## 📁 Estrutura

| Característica | Implementação |
|----------------|---------------|
| Captura | DXGI Desktop Duplication API (GPU-native) |
| Renderização | Direct3D 11, quad fullscreen, sem vertex buffer |
| Escala | Bilinear na GPU (4K → 1080p) |
| Cursor | Capturado via DXGI (tipos: COLOR, MONOCHROME, MASKED) |
| SwapChain | `FLIP_DISCARD` + `ALLOW_TEARING` |
| Latência | Sem vsync; `Present(0, ALLOW_TEARING)` |
| Janela | `WS_POPUP + WS_EX_TOPMOST` — fullscreen sem bordas |
| Frequência máx | Ilimitada (sem vsync), depende da GPU e do driver |

### Por que DXGI Desktop Duplication?

- **Sem cópia CPU**: o frame fica na GPU o tempo todo, do capture ao present
- **Suporte nativo a 4K**: sem limitações de resolução
- **Alta frequência**: funciona em 144 Hz, 240 Hz, etc.
- **Padrão do Windows**: usado por OBS, RDP, shadow-copy, etc.

---

## 🎮 Caso de uso: VDD 4K → Monitor 1080p 144 Hz

1. Configure o jogo para renderizar na tela VDD (4K)
2. Abra o ScreenMirror
3. **ORIGEM** = VDD 4K  /  **DESTINO** = Monitor 1080p 144 Hz
4. O jogo aparece no seu monitor físico em tempo real, com escala GPU e cursor visível

---

## 🐛 Troubleshooting

**"DuplicateOutput falhou"**
- Certifique-se de que não há outra instância rodando
- Não funciona sobre conexão RDP — requer sessão local
- Verifique se o driver da GPU suporta DXGI 1.2 (Windows 8+)

**Tela preta / sem imagem**
- Verifique se a tela de origem está ativa e com conteúdo sendo exibido
- Alguns jogos em "Fullscreen Exclusivo" podem bloquear a captura; use "Borderless Windowed"

**Não aparece no executável**
- Verifique se o build foi bem-sucedido (sem erros no `build.bat`)
- Assegure-se de usar o compilador x64

---

---

## 📁 Estrutura

```
ScreenMirror/
├── src/
│   └── main.cpp        # Código-fonte completo (~500 linhas)
├── bin/                # Executável gerado pelo build
├── build.bat           # Script de build automático (MSVC)
├── CMakeLists.txt      # Build alternativo via CMake
└── README.md           # Este arquivo
```
