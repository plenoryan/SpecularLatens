# SpecularLatens

Espelhamento de tela com **baixíssima latência** via **DXGI Desktop Duplication API + Direct3D 11**.

Captura qualquer monitor (incluindo displays virtuais como VDD) e exibe em outro em tempo real — sem cópias desnecessárias, tudo na GPU.

---

## 🎮 Caso de Uso: Simulando Super Resolução (VSR / DSR / DLSS)

Este programa foi criado especificamente para facilitar a simulação de altas resoluções em monitores que não as suportam nativamente. 

### O Fluxo:
1.  **Tela Virtual:** Utilize um driver como o **VDD (Virtual Display Driver)** para criar uma tela virtual de alta resolução (ex: 4K) no seu sistema.
2.  **Execução do Jogo:** Configure o seu jogo para rodar nesta tela virtual em 4K.
3.  **Visualização:** Utilize o **SpecularLatens** para capturar essa tela virtual 4K e exibi-la em tempo real no seu monitor físico de menor resolução (ex: 1080p).
4.  **Resultado:** Você consegue rodar o jogo em uma resolução muito maior, aproveitando o sampling e a fidelidade visual da Super Resolução, visualizando tudo no seu monitor principal com baixíssima latência.

---

## 🚀 Download Direto

Para usuários que não são desenvolvedores, baixe a versão pronta para uso abaixo:

**[➔ Baixar SpecularLatens v1.00](https://github.com/plenoryan/SpecularLatens/releases/download/v1.00/SpecularLatens.exe)**

*(Basta baixar, extrair e rodar o SpecularLatens.exe)*

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
cd C:\Developer\SpecularLatens
build.bat
```

O executável será gerado em `bin\SpecularLatens.exe`.

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
cd C:\Developer\SpecularLatens
cl /nologo /std:c++17 /O2 /EHsc /D UNICODE /D _UNICODE ^
   /Fe bin\SpecularLatens.exe src\main.cpp ^
   /link /SUBSYSTEM:WINDOWS ^
   d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib
```

---

## ▶️ Como usar

1. Execute `bin\SpecularLatens.exe`
2. Selecione a **ORIGEM** (tela a capturar — ex.: seu VDD 4K)
3. Selecione o **DESTINO** (tela onde exibir — ex.: seu monitor 1080p 144 Hz)
4. Clique em **"Iniciar Espelhamento"**
5. Pressione **ESC** para sair

---

## ⚙️ Detalhes técnicos
... (omitted content for clarity, I'll provide full replacement) ...
### 📦 Portabilidade

O **SpecularLatens** é totalmente portátil. O executável gerado em `bin/SpecularLatens.exe` é um arquivo único e autossuficiente:
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
2. Abra o SpecularLatens
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

## ⚙️ Especificações de Performance e Limitações Técnicas

Para alcançar nossa cobiçada latência "zero", o **ScreenMirror** entrega um funcionamento direto e cru, com as seguintes características em setups avançados de alta resolução:

*   **Gerenciamento de Cor (HDR/10-bit):** Focado na compatibilidade primária e estabilidade, o processo via *DXGI Desktop Duplication* captura texturas de formato padrão de 8-bits SDR (`B8G8R8A8_UNORM`). Jogar com fontes nativas 4K HDR resultará na degradação das cores intensas (podendo aparentar estar "lavadas" no destino SDR).
*   **Encaminhamento de Áudio:** O App trabalha 100% no circuito de processamento visual (GPU). No entanto, **utilizando o Virtual Display Driver (VDD)** recomendado no processo, **não há problemas de áudio ou dessincronização**, uma vez que a instalação do VDD acompanha seu próprio canal de áudio que fará a ponte sonora fluir corretamente em seu sistema, sem exigir aplicativos extras.
*   **Controle de Input:** É projetado como um "espelho estrito". Não tem a função de resgatar seus cliques, redimensionamento ou digitação para uma injeção de evento no monitor origem remotamente. Toda interação continua baseada na operação nativa dentro do seu Windows local.
*   **Eficiência de CPU e Hardware:** Não possui peso perceptível de CPU em escalas 4K e não gasta o hardware com codificação convencional comutável (como NVIDIA NVENC ou AMD AMF). Aproveitando métodos transparentes nativos, o Direct3D 11 realiza uma técnica `Zero-Copy` sem mover dados para re-codificar em vídeo mp4/stream, tirando completamente o fardo do processador durante os jogos.
*   **Frame Generation / "Sem-Frescura":** O processo é puramente raw 1:1, copiando integralmente a imagem de saída de forma imediata (sem frame-buffer extra). Não introduz recursos virtuais tipo Lossless Scaling ou "Upscalers Interpolados de IA", favorecendo latência zero em troca de efeitos cosméticos.
*   **Uso Locar/Modular:** O fluxo captura o buffer alocado fisicamente direto do barramento da GPU. Sua excelência e estabilidade dependem de conexão estrita via placa gráfica em modo local (Cabo HDMI / DP / Display Virtual). Este não é um app de cast para a rede (Wi-Fi/LAN).

---

## 📁 Estrutura

```
SpecularLatens/
├── src/
│   └── main.cpp        # Código-fonte completo
├── bin/                # Executável gerado pelo build
├── build.bat           # Script de build automático (MSVC)
├── CMakeLists.txt      # Build alternativo via CMake
└── README.md           # Este arquivo
```
