# Memórias

Um site pessoal para guardar fotos (com legenda), textos e recados, com um
fundo de origamis caindo. Feito com **HTML, CSS e JavaScript** no front-end,
e um **servidor em C puro** (sem frameworks) como backend opcional.


**Fotos e textos são separados por perfil** — cada um só vê os seus.
**Os recados são compartilhados**: os dois veem o mesmo mural, e cada
recado aparece assinado automaticamente com o nome de quem estava
logado ao escrever.

⚠️ Como isso é um site estático, usuário e senha ficam visíveis em texto
puro dentro do `script.js` — qualquer um que veja o código-fonte ou o
repositório consegue ler. Isso é suficiente pra afastar visitantes
aleatórios, mas não é segurança de verdade. Se quiser mais privacidade,
deixe o repositório do GitHub como **privado**.

## Estrutura

```
index.html        página principal (tela de login + o site)
style.css         estilo (paleta, layout, animação dos origamis)
script.js         login, abas, formulários e armazenamento
server/
  server.c        servidor HTTP em C (sockets), API para fotos/textos/recados
  Makefile        compila o servidor
```

## Modo 1 — só o site, no GitHub Pages (mais simples)

Suba `index.html`, `style.css` e `script.js` pro GitHub Pages normalmente
(Settings → Pages → escolher a branch). O site funciona sozinho: cada
foto e texto fica salvo no `localStorage` do navegador, separado por
perfil (`fotos_amanda`, `textos_caetano`, etc.). Os recados também usam
o `localStorage`, mas como não há servidor, só ficam "compartilhados de
verdade" se os dois usarem o mesmo navegador/aparelho.

**Limitação:** o que a Amanda salva no celular dela só aparece no
celular dela; o que o Caetano salva no notebook dele só aparece lá. Não
existe servidor rodando, então não tem como sincronizar sozinho — o
GitHub Pages só entrega arquivos, ele não executa nada (nem C, nem PHP,
nem Node).

## Modo 2 — com o servidor em C, salvando em qualquer aparelho

Se vocês quiserem que os dados (inclusive o mural de recados) sejam
realmente os mesmos em qualquer aparelho, precisam rodar
`server/server.c` em algum lugar que fique ligado e acessível (o
computador de um dos dois na mesma rede, ou uma VPS com IP público).

### Compilar e rodar

```bash
cd server
make
./servidor
```

Isso sobe um servidor em `http://localhost:8080`, e cria uma pasta
`server/data/` com os arquivos de fotos, textos (cada um numa subpasta
por usuário) e recados (compartilhados) — é o "banco de dados", cada
item vira um arquivo.

### Ligar o site a esse servidor

Abra `script.js` e mude a primeira linha:

```js
const API_BASE = ""; // antes
const API_BASE = "http://localhost:8080"; // ou o endereço do seu servidor
```

Se o site estiver no GitHub Pages e o servidor no computador de alguém,
isso só funciona enquanto esse computador estiver ligado, com o
servidor rodando, e acessível pela internet (normalmente precisa
liberar a porta 8080 no roteador, ou usar algo como um túnel/VPS). Pra
uso entre os dois, mais simples é rodar site e servidor na mesma rede
local, ou colocar o servidor numa VPS barata.

### Como o servidor guarda os dados

Sem bibliotecas de banco de dados nem JSON de verdade — de propósito,
pra manter o C simples de ler:

- cada foto vira dois arquivos: `data/fotos/<usuario>/<id>.meta`
  (legenda + data) e `data/fotos/<usuario>/<id>.img` (a imagem em
  base64, como texto puro)
- cada texto vira `data/textos/<usuario>/<id>.meta` e
  `data/textos/<usuario>/<id>.txt`
- cada recado (compartilhado, sem separação por usuário) vira
  `data/recados/<id>.meta` e `data/recados/<id>.txt`

`<usuario>` só pode ser `amanda` ou `caetano` — o servidor rejeita
qualquer outro valor (código 403), o que também evita que alguém tente
usar `../` no nome pra ler arquivos fora da pasta esperada.

A comunicação entre o navegador e o servidor usa um formato de texto
simples (não JSON), por exemplo, para criar um texto:

```
TITULO=meu%20dia
DATA=2026-07-22T10:00:00.000Z
---CONTEUDO---
Hoje foi um dia bom.
```

O servidor responde em JSON (montado manualmente, com escape de aspas e
quebras de linha) pra o `script.js` conseguir ler com `fetch(...).json()`
normalmente.

### Rotas da API

| Método | Rota                        | Faz o quê                              |
|--------|------------------------------|------------------------------------------|
| GET    | `/api/fotos/<usuario>`        | lista as fotos daquele usuário            |
| POST   | `/api/fotos/<usuario>`        | adiciona uma foto pra aquele usuário      |
| DELETE | `/api/fotos/<usuario>/<id>`   | apaga uma foto daquele usuário            |
| GET    | `/api/textos/<usuario>`       | lista os textos daquele usuário           |
| POST   | `/api/textos/<usuario>`       | adiciona um texto pra aquele usuário      |
| DELETE | `/api/textos/<usuario>/<id>`  | apaga um texto daquele usuário            |
| GET    | `/api/recados`                | lista todos os recados (compartilhado)    |
| POST   | `/api/recados`                | adiciona um recado (compartilhado)        |
| DELETE | `/api/recados/<id>`           | apaga um recado (compartilhado)           |



## Por que não dá pra rodar o C dentro do GitHub Pages

GitHub Pages serve arquivos estáticos (HTML, CSS, JS, imagens) direto do
repositório — ele não tem um processo rodando por trás capaz de compilar
ou executar C, Java, Python, PHP etc. Qualquer linguagem de servidor
precisa de uma máquina real ligada (seu PC, uma VPS, um serviço de
hospedagem com backend). Por isso o projeto foi dividido assim: o que é
estático vai pro GitHub Pages, e o que precisa "rodar" fica no
`server/`, pra você usar quando e onde quiser.
