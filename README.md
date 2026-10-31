# LimpaVoz (VST3)

Plugin de redução de ruído para voz. Usa o RNNoise (rede neural da Xiph/Mozilla, licença BSD)
para deixar só a voz e tirar chiado, ventilador, ar-condicionado, ruído de fundo etc.

Controles:
- **Intensidade da limpeza** (0–100 %): 100 % = só a voz limpa; valores menores misturam um pouco do original.
- **Corte de graves** (20–300 Hz): filtro passa-altas para tirar estrondos e vibrações. 20 Hz = desligado.
- **Ganho de saída** (−12 a +12 dB).
- **Bypass**: desliga a limpeza (mantém a latência, então não desalinha a mixagem).

Latência: ~30 ms a 48 kHz (o plugin avisa o DAW, que compensa sozinho).
Funciona em mono e estéreo, em qualquer taxa de amostragem (reamostra internamente para 48 kHz).

## Como gerar o .vst3 para Windows (sem instalar nada no seu PC)

1. Crie uma conta/repositório no GitHub (pode ser privado) e envie esta pasta inteira.
2. Abra a aba **Actions** do repositório. O workflow "Compilar VST3 (Windows)" roda sozinho a cada envio
   (ou clique em *Run workflow*). Leva uns 5–10 minutos.
3. Quando terminar, abra a execução e baixe o artefato **LimpaVoz-VST3-Windows**.
4. Descompacte e copie a pasta **LimpaVoz.vst3** para `C:\Program Files\Common Files\VST3`.
5. Reescaneie os plugins no seu DAW.

## Requisitos

- Windows 64 bits e CPU com AVX2 (qualquer Ryzen/Intel dos últimos ~10 anos).

## Testes do núcleo

`tests/test_core.cpp` verifica latência, alinhamento dry/wet e redução de ruído em 44,1 / 48 / 96 kHz.

## Licenças

RNNoise: BSD (veja `third_party/rnnoise/COPYING`). JUCE: AGPLv3/comercial — uso pessoal tudo bem;
para vender o plugin é preciso respeitar a licença do JUCE.
