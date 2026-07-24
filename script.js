/* =====================================================================
   CONFIGURAÇÃO
   Deixe API_BASE vazio ("") para o site funcionar sozinho no GitHub Pages,
   salvando cada aparelho separadamente (localStorage do navegador).

   Se você compilar e rodar o servidor em C (pasta /server) e ele estiver
   acessível por uma URL (local ou de uma VPS), coloque essa URL aqui,
   por exemplo: "http://192.168.0.10:8080" ou "http://meudominio.com:8080".
   Aí sim os dados passam a ser salvos de verdade e aparecem em qualquer
   aparelho que abrir o site — inclusive os recados compartilhados entre
   os dois perfis.
===================================================================== */
const API_BASE = ""; // ex: "http://localhost:8080"

/* =====================================================================
   USUÁRIOS
   Login simples, só para duas pessoas. Atenção: como este é um site
   estático, usuário e senha ficam visíveis no código-fonte para quem
   olhar o JS ou o repositório — isso mantém estranhos de fora, mas não
   é segurança de verdade. Se quiser mais privacidade, deixe o
   repositório do GitHub como privado.
===================================================================== */
const USUARIOS = [
  { usuario: 'amanda',  senha: 'geladeira', nome: 'Amanda'  },
  { usuario: 'caetano', senha: 'roxo',      nome: 'Caetano' },
];
const CHAVE_SESSAO = 'memorias_sessao';
let usuarioAtual = null; // preenchido após o login

/* ============ ORIGAMIS CAINDO INFINITAMENTE ============ */
(function origamis(){
  const camada = document.getElementById('origami-layer');
  const tons = [
    ['#8C5A72', '#6f425a', '#4f2e40'],
    ['#6E7F5C', '#566447', '#3c4732'],
    ['#C9A15A', '#a9813f', '#7d5f2c'],
    ['#7C93A8', '#5f7488', '#42505d'],
  ];
  function criarOrigami(){
    const el = document.createElement('div');
    el.className = 'origami';
    const paleta = tons[Math.floor(Math.random()*tons.length)];
    el.style.setProperty('--shade-light', paleta[0]);
    el.style.setProperty('--shade-mid', paleta[1]);
    el.style.setProperty('--shade-dark', paleta[2]);
    const tam = 16 + Math.random()*22;
    el.style.width = tam + 'px';
    el.style.height = tam + 'px';
    el.style.left = (Math.random()*100) + 'vw';
    const duracao = 9 + Math.random()*10;
    el.style.animationDuration = duracao + 's';
    el.style.setProperty('--drift', (Math.random()*140 - 70) + 'px');
    el.style.setProperty('--spin', (Math.random()*520 - 260) + 'deg');
    el.style.opacity = (0.35 + Math.random()*0.35).toFixed(2);
    camada.appendChild(el);
    setTimeout(()=> el.remove(), duracao*1000 + 200);
  }
  for(let i=0;i<8;i++){ setTimeout(criarOrigami, i*400); }
  setInterval(criarOrigami, 750);
})();

/* ============ ABAS ============ */
document.querySelectorAll('.aba-btn').forEach(btn=>{
  btn.addEventListener('click', ()=>{
    document.querySelectorAll('.aba-btn').forEach(b=>b.classList.remove('ativa'));
    document.querySelectorAll('.painel').forEach(p=>p.classList.remove('ativo'));
    btn.classList.add('ativa');
    document.getElementById('painel-' + btn.dataset.aba).classList.add('ativo');
  });
});

/* ============ HELPERS ============ */
function novoId(){
  return Date.now().toString(36) + Math.random().toString(36).slice(2,8);
}
function dataAmigavel(iso){
  try{
    return new Date(iso).toLocaleDateString('pt-BR', {day:'2-digit', month:'short', year:'numeric'});
  }catch(e){ return ''; }
}
function escapeHtml(str){
  return (str||'').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
function lsGet(key){
  try{ const v = localStorage.getItem(key); return v ? JSON.parse(v) : null; }
  catch(e){ return null; }
}
function lsSet(key, value){
  try{ localStorage.setItem(key, JSON.stringify(value)); return true; }
  catch(e){ console.error('erro ao salvar localmente', e); return false; }
}

/* =====================================================================
   LOGIN
===================================================================== */
function iniciarSessao(usuario){
  usuarioAtual = usuario;
  localStorage.setItem(CHAVE_SESSAO, usuario.usuario);
  document.getElementById('tela-login').hidden = true;
  document.getElementById('app').hidden = false;
  document.getElementById('nome-usuario').textContent = usuario.nome;
  carregarFotos();
  carregarTextos();
  carregarRecados();
}

document.getElementById('form-login').addEventListener('submit', (e)=>{
  e.preventDefault();
  const usuarioDigitado = document.getElementById('login-usuario').value.trim().toLowerCase();
  const senhaDigitada = document.getElementById('login-senha').value;
  const status = document.getElementById('status-login');
  const encontrado = USUARIOS.find(u => u.usuario === usuarioDigitado && u.senha === senhaDigitada);
  if(!encontrado){
    status.textContent = 'usuário ou senha incorretos';
    return;
  }
  status.textContent = '';
  iniciarSessao(encontrado);
});

document.getElementById('btn-sair').addEventListener('click', ()=>{
  localStorage.removeItem(CHAVE_SESSAO);
  location.reload();
});

/* tenta retomar sessão já existente neste navegador */
(function retomarSessao(){
  const usuarioSalvo = localStorage.getItem(CHAVE_SESSAO);
  if(!usuarioSalvo) return;
  const encontrado = USUARIOS.find(u => u.usuario === usuarioSalvo);
  if(encontrado) iniciarSessao(encontrado);
})();

/* =====================================================================
   CAMADA DE ARMAZENAMENTO
   Fotos e textos são separados por perfil (cada um só vê o que é seu).
   Recados são compartilhados entre os dois perfis.
   Cada função tenta usar o servidor em C (se API_BASE estiver definido);
   caso contrário usa o localStorage do navegador.
===================================================================== */
const Armazenamento = {

  async listarFotos(){
    if(API_BASE){
      const r = await fetch(`${API_BASE}/api/fotos/${usuarioAtual.usuario}`);
      if(!r.ok) throw new Error('falha ao buscar fotos');
      return await r.json();
    }
    return lsGet('fotos_' + usuarioAtual.usuario) || [];
  },
  async adicionarFoto(legenda, imagemBase64){
    const registro = { id: novoId(), legenda, imagem: imagemBase64, data: new Date().toISOString() };
    if(API_BASE){
      const corpo = `LEGENDA=${encodeURIComponent(legenda)}\nDATA=${registro.data}\n---IMG_BASE64---\n${imagemBase64.split(',')[1] || imagemBase64}`;
      const r = await fetch(`${API_BASE}/api/fotos/${usuarioAtual.usuario}`, { method:'POST', headers:{'Content-Type':'text/plain'}, body: corpo });
      if(!r.ok) throw new Error('falha ao salvar foto');
      return await r.json();
    }
    const chave = 'fotos_' + usuarioAtual.usuario;
    const lista = lsGet(chave) || [];
    lista.push(registro);
    if(!lsSet(chave, lista)) throw new Error('sem espaço no armazenamento local');
    return registro;
  },
  async apagarFoto(id){
    if(API_BASE){
      await fetch(`${API_BASE}/api/fotos/${usuarioAtual.usuario}/${id}`, { method:'DELETE' });
      return;
    }
    const chave = 'fotos_' + usuarioAtual.usuario;
    const lista = (lsGet(chave) || []).filter(f => f.id !== id);
    lsSet(chave, lista);
  },

  async listarTextos(){
    if(API_BASE){
      const r = await fetch(`${API_BASE}/api/textos/${usuarioAtual.usuario}`);
      if(!r.ok) throw new Error('falha ao buscar textos');
      return await r.json();
    }
    return lsGet('textos_' + usuarioAtual.usuario) || [];
  },
  async adicionarTexto(titulo, conteudo){
    const registro = { id: novoId(), titulo, conteudo, data: new Date().toISOString() };
    if(API_BASE){
      const corpo = `TITULO=${encodeURIComponent(titulo)}\nDATA=${registro.data}\n---CONTEUDO---\n${conteudo}`;
      const r = await fetch(`${API_BASE}/api/textos/${usuarioAtual.usuario}`, { method:'POST', headers:{'Content-Type':'text/plain'}, body: corpo });
      if(!r.ok) throw new Error('falha ao salvar texto');
      return await r.json();
    }
    const chave = 'textos_' + usuarioAtual.usuario;
    const lista = lsGet(chave) || [];
    lista.push(registro);
    lsSet(chave, lista);
    return registro;
  },
  async apagarTexto(id){
    if(API_BASE){
      await fetch(`${API_BASE}/api/textos/${usuarioAtual.usuario}/${id}`, { method:'DELETE' });
      return;
    }
    const chave = 'textos_' + usuarioAtual.usuario;
    const lista = (lsGet(chave) || []).filter(t => t.id !== id);
    lsSet(chave, lista);
  },

  /* recados: compartilhados entre os dois perfis */
  async listarRecados(){
    if(API_BASE){
      const r = await fetch(API_BASE + '/api/recados');
      if(!r.ok) throw new Error('falha ao buscar recados');
      return await r.json();
    }
    return lsGet('recados') || [];
  },
  async adicionarRecado(mensagem){
    const registro = { id: novoId(), nome: usuarioAtual.nome, mensagem, data: new Date().toISOString() };
    if(API_BASE){
      const corpo = `NOME=${encodeURIComponent(usuarioAtual.nome)}\nDATA=${registro.data}\n---MENSAGEM---\n${mensagem}`;
      const r = await fetch(API_BASE + '/api/recados', { method:'POST', headers:{'Content-Type':'text/plain'}, body: corpo });
      if(!r.ok) throw new Error('falha ao salvar recado');
      return await r.json();
    }
    const lista = lsGet('recados') || [];
    lista.push(registro);
    lsSet('recados', lista);
    return registro;
  },
  async apagarRecado(id){
    if(API_BASE){
      await fetch(API_BASE + '/api/recados/' + id, { method:'DELETE' });
      return;
    }
    const lista = (lsGet('recados') || []).filter(r => r.id !== id);
    lsSet('recados', lista);
  }
};

/* ============ FOTOS ============ */
async function comprimirImagem(file){
  return new Promise((resolve, reject)=>{
    const reader = new FileReader();
    reader.onload = (e)=>{
      const img = new Image();
      img.onload = ()=>{
        const maxLado = 900;
        let {width, height} = img;
        if(width > height && width > maxLado){
          height = Math.round(height * (maxLado/width));
          width = maxLado;
        }else if(height > maxLado){
          width = Math.round(width * (maxLado/height));
          height = maxLado;
        }
        const canvas = document.createElement('canvas');
        canvas.width = width; canvas.height = height;
        const ctx = canvas.getContext('2d');
        ctx.drawImage(img, 0, 0, width, height);
        resolve(canvas.toDataURL('image/jpeg', 0.75));
      };
      img.onerror = reject;
      img.src = e.target.result;
    };
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });
}

async function carregarFotos(){
  const mural = document.getElementById('mural-fotos');
  mural.innerHTML = '<p class="vazio">carregando fotos...</p>';
  try{
    const fotos = await Armazenamento.listarFotos();
    if(!fotos.length){
      mural.innerHTML = '<p class="vazio">nenhuma foto guardada ainda</p>';
      return;
    }
    mural.innerHTML = '';
    fotos.slice().reverse().forEach(foto=>{
      const src = foto.imagem.startsWith('data:') ? foto.imagem : ('data:image/jpeg;base64,' + foto.imagem);
      const div = document.createElement('div');
      div.className = 'polaroid';
      div.innerHTML = `
        <button class="apagar" title="apagar" data-id="${foto.id}">✕</button>
        <img src="${src}" alt="${escapeHtml(foto.legenda || 'foto guardada')}">
        <div class="legenda">${escapeHtml(foto.legenda || '')}</div>
        <div class="data">${dataAmigavel(foto.data)}</div>
      `;
      mural.appendChild(div);
    });
    mural.querySelectorAll('.apagar').forEach(b=>{
      b.addEventListener('click', async ()=>{ await Armazenamento.apagarFoto(b.dataset.id); carregarFotos(); });
    });
  }catch(e){
    mural.innerHTML = '<p class="vazio">não consegui carregar as fotos agora</p>';
  }
}

document.getElementById('btn-add-foto').addEventListener('click', async ()=>{
  const input = document.getElementById('foto-arquivo');
  const legenda = document.getElementById('foto-legenda').value.trim();
  const status = document.getElementById('status-foto');
  if(!input.files || !input.files[0]){
    status.textContent = 'escolha uma imagem primeiro';
    return;
  }
  status.textContent = 'guardando...';
  try{
    const imagem = await comprimirImagem(input.files[0]);
    await Armazenamento.adicionarFoto(legenda, imagem);
    input.value = '';
    document.getElementById('foto-legenda').value = '';
    status.textContent = 'foto guardada!';
    setTimeout(()=> status.textContent = '', 2000);
    carregarFotos();
  }catch(e){
    status.textContent = 'não consegui salvar: ' + e.message;
  }
});

/* ============ TEXTOS ============ */
async function carregarTextos(){
  const lista = document.getElementById('lista-textos');
  lista.innerHTML = '<p class="vazio">carregando textos...</p>';
  try{
    const textos = await Armazenamento.listarTextos();
    if(!textos.length){
      lista.innerHTML = '<p class="vazio">nenhum texto escrito ainda</p>';
      return;
    }
    lista.innerHTML = '';
    textos.slice().reverse().forEach(t=>{
      const div = document.createElement('div');
      div.className = 'pagina';
      div.innerHTML = `
        <button class="apagar" title="apagar" data-id="${t.id}">✕</button>
        <h4>${escapeHtml(t.titulo)}</h4>
        <div class="data">${dataAmigavel(t.data)}</div>
        <p>${escapeHtml(t.conteudo)}</p>
      `;
      lista.appendChild(div);
    });
    lista.querySelectorAll('.apagar').forEach(b=>{
      b.addEventListener('click', async ()=>{ await Armazenamento.apagarTexto(b.dataset.id); carregarTextos(); });
    });
  }catch(e){
    lista.innerHTML = '<p class="vazio">não consegui carregar os textos agora</p>';
  }
}

document.getElementById('btn-add-texto').addEventListener('click', async ()=>{
  const titulo = document.getElementById('texto-titulo').value.trim();
  const conteudo = document.getElementById('texto-conteudo').value.trim();
  const status = document.getElementById('status-texto');
  if(!titulo || !conteudo){
    status.textContent = 'escreva um título e um texto';
    return;
  }
  status.textContent = 'salvando...';
  try{
    await Armazenamento.adicionarTexto(titulo, conteudo);
    document.getElementById('texto-titulo').value = '';
    document.getElementById('texto-conteudo').value = '';
    status.textContent = 'texto salvo!';
    setTimeout(()=> status.textContent = '', 2000);
    carregarTextos();
  }catch(e){
    status.textContent = 'não consegui salvar: ' + e.message;
  }
});

/* ============ RECADOS ============ */
async function carregarRecados(){
  const mural = document.getElementById('mural-recados');
  mural.innerHTML = '<p class="vazio">carregando recados...</p>';
  try{
    const recados = await Armazenamento.listarRecados();
    if(!recados.length){
      mural.innerHTML = '<p class="vazio">nenhum recado ainda</p>';
      return;
    }
    mural.innerHTML = '';
    recados.slice().reverse().forEach(r=>{
      const div = document.createElement('div');
      div.className = 'bilhete';
      div.innerHTML = `
        <button class="apagar" title="apagar" data-id="${r.id}">✕</button>
        <div>${escapeHtml(r.mensagem)}</div>
        <div class="autor">${escapeHtml(r.nome || 'anônimo')} · ${dataAmigavel(r.data)}</div>
      `;
      mural.appendChild(div);
    });
    mural.querySelectorAll('.apagar').forEach(b=>{
      b.addEventListener('click', async ()=>{ await Armazenamento.apagarRecado(b.dataset.id); carregarRecados(); });
    });
  }catch(e){
    mural.innerHTML = '<p class="vazio">não consegui carregar os recados agora</p>';
  }
}

document.getElementById('btn-add-recado').addEventListener('click', async ()=>{
  const mensagem = document.getElementById('recado-msg').value.trim();
  const status = document.getElementById('status-recado');
  if(!mensagem){
    status.textContent = 'escreva um recado';
    return;
  }
  status.textContent = 'salvando...';
  try{
    await Armazenamento.adicionarRecado(mensagem);
    document.getElementById('recado-msg').value = '';
    status.textContent = 'recado deixado!';
    setTimeout(()=> status.textContent = '', 2000);
    carregarRecados();
  }catch(e){
    status.textContent = 'não consegui salvar: ' + e.message;
  }
});
