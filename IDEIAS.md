# Ideias para o ToneForge

Anotado em 23/10/2026. Nada disso foi implementado ainda.

## 1. Suporte ao TONE3000

O TONE3000 não tem presets de configuração. O que ele oferece são **capturas NAM**
(arquivos `.nam`, redes neurais que imitam um amp ou pedal real) e **IRs** de gabinete.

- **IRs:** já funcionam hoje. Use o botão "Carregar IR (.wav)" no bloco de gabinete.
- **Capturas NAM:** falta um bloco novo, "Amp NAM", que roda o arquivo `.nam`.
  - Usaria a biblioteca NeuralAmpModelerCore (C++, MIT) com Eigen.
  - Formatos: A1 (WaveNet/LSTM) e o A2, que é mais novo.
  - Reamostragem quando a taxa do arquivo for diferente da do áudio (normalmente 48 kHz).
- **Busca dentro do app:** usar a API oficial do TONE3000.
  - Exige login do usuário pelo navegador (OAuth com PKCE).
  - Exige uma chave de desenvolvedor registrada no TONE3000.
  - Licença T3K: pode usar e gravar com os arquivos, mas não pode redistribuir.
    Então o app baixa sob demanda e não embute capturas.

## 2. Assistente de timbre com IA

O usuário pede, por exemplo, "timbre de Back in Black", e a IA monta o preset.

- A IA (Claude, pela API da Anthropic) recebe três coisas:
  - o pedido;
  - a lista de módulos e parâmetros do ToneForge (já existe no registro de módulos);
  - as capturas e IRs disponíveis.
- Ela devolve um preset em JSON, no mesmo formato de hoje. O app valida e carrega.
- Dá para refinar conversando: "mais grave", "menos ganho", "o solo".
- **Limite:** a IA se baseia no que se sabe do equipamento usado na gravação, não
  ouve a música. Guitarra, captador e mixagem pesam muito no resultado.
- **Requisitos:** internet e a chave de API do próprio usuário, guardada com
  proteção do Windows (DPAPI). Cada pedido custa centavos.

## 3. Tone Match (complemento do item 2)

- O usuário fornece um trecho da guitarra isolada da música.
- O app compara o espectro desse trecho com o da saída do ToneForge e ajusta um
  EQ automaticamente.
- É DSP clássico e não precisa de IA. Combinado com o assistente e uma captura
  NAM do mesmo amp, chega bem mais perto do timbre original.

## 4. Afinador (a definir)

Pedido em 23/10/2026: "inserir um afinador".

Já existe um afinador: botão **Afinador** na barra de cima ou tecla `T`. Ele abre
numa janela flutuante, com nota, cents, referência A4 e opção de silenciar a saída.
Falta confirmar o que você quer além disso. Possibilidades:

- afinador sempre visível na tela principal, sem abrir janela;
- afinador como pedal dentro da cadeia, ligado por um footswitch MIDI;
- afinador polifônico (todas as cordas de uma vez);
- afinações alternativas (Drop D, meio tom abaixo etc.) com a corda-alvo indicada;
- visual maior, tipo pedal de palco, para ler de longe.

## Ordem sugerida

1. Bloco Amp NAM carregando arquivos `.nam` baixados à mão: maior ganho de realismo.
2. Assistente de IA usando os módulos atuais (independente do item 1).
3. Navegador do TONE3000 dentro do app (API + login).
4. Tone Match com áudio de referência.
