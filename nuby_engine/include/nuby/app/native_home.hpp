#pragma once
#include <string>

namespace nuby::app {
// UI moderna servida por el proceso C++ de Nuby: HTML/CSS/JS nativo, sin canvas.
inline std::string native_home_html() {
    return R"NUBY_NATIVE(<!doctype html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <meta name="theme-color" content="#ffffff">
  <title>Nuby</title>
  <style>
    :root { --text:#202124; --muted:#5f6368; --line:#dfe1e5; --blue:#1a73e8; --blue-soft:#e8f0fe; --white:#fff; font-family:"Segoe UI",Roboto,system-ui,sans-serif; }
    * { box-sizing:border-box; } html,body { min-height:100%; } body { margin:0; color:var(--text); background:#fff; }
    button,input { font:inherit; } button { -webkit-tap-highlight-color:transparent; }
    .page { min-height:100dvh; display:flex; flex-direction:column; }
    header { height:68px; display:flex; align-items:center; padding:0 clamp(20px,4vw,48px); }
    .logo { color:var(--text); text-decoration:none; font-size:24px; font-weight:300; letter-spacing:-.5px; }
    main { flex:1; display:flex; flex-direction:column; justify-content:center; align-items:center; padding:24px 20px 38px; }
    .search-area { width:min(584px,100%); text-align:center; transform:translateY(-5%); }
    .search-box { display:flex; align-items:center; width:100%; height:48px; border:1px solid var(--line); border-radius:24px; background:var(--white); transition:box-shadow .2s,border-color .2s; }
    .search-box:hover,.search-box:focus-within { border-color:transparent; box-shadow:0 1px 6px rgba(32,33,36,.28); }
    .search-label { flex:0 0 auto; margin-left:14px; color:var(--muted); font-size:14px; }
    .search-box input { flex:1; min-width:0; height:100%; border:0; outline:0; padding:0 12px; color:var(--text); background:transparent; font-size:16px; }
    .search-box input::placeholder { color:var(--muted); opacity:1; }
    .microphone { flex:0 0 auto; margin-right:14px; border:0; padding:5px; color:var(--muted); background:transparent; cursor:pointer; font-size:13px; }
    .microphone:hover,.microphone:focus-visible { color:var(--blue); outline:0; }
    .description { margin:20px 0 0; color:var(--muted); font-size:16px; line-height:1.45; }
    footer { position:relative; padding:18px 20px max(24px,env(safe-area-inset-bottom)); text-align:center; }
    .nav-lines { display:flex; flex-direction:column; align-items:center; gap:13px; }
    .nav-line { display:flex; flex-wrap:wrap; justify-content:center; column-gap:40px; row-gap:10px; }
    .nav-link { border:0; padding:0; color:var(--muted); background:transparent; cursor:pointer; font-size:14px; line-height:1.35; transition:color .2s; }
    .nav-link:hover,.nav-link:focus-visible { color:var(--blue); text-decoration:underline; outline:0; }
    .menu-card { position:fixed; z-index:20; left:50%; bottom:132px; width:min(330px,calc(100vw - 32px)); padding:8px; border-radius:8px; background:#fff; box-shadow:0 4px 20px rgba(0,0,0,.15); opacity:0; visibility:hidden; pointer-events:none; transform:translate(-50%,9px); transition:opacity .2s,transform .2s,visibility .2s; text-align:left; }
    .menu-card.open { opacity:1; visibility:visible; pointer-events:auto; transform:translate(-50%,0); }
    .menu-title { padding:9px 11px 6px; color:var(--muted); font-size:12px; font-weight:600; letter-spacing:.4px; text-transform:uppercase; }
    .menu-item { display:block; width:100%; border:0; border-radius:5px; padding:10px 11px; color:var(--text); background:transparent; cursor:pointer; text-align:left; font-size:14px; transition:background .2s,color .2s; }
    .menu-item:hover,.menu-item:focus-visible { color:var(--blue); background:var(--blue-soft); outline:0; }
    .modal { position:fixed; z-index:30; inset:0; display:grid; place-items:center; padding:20px; background:rgba(0,0,0,.4); opacity:0; visibility:hidden; transition:opacity .2s,visibility .2s; }
    .modal.open { opacity:1; visibility:visible; }
    .modal-card { position:relative; width:min(390px,100%); padding:31px 30px; border-radius:10px; background:#fff; box-shadow:0 4px 20px rgba(0,0,0,.15); text-align:center; transform:scale(.96); transition:transform .2s; }
    .modal.open .modal-card { transform:scale(1); } .modal-card h2 { margin:0 0 13px; color:var(--text); font-size:32px; font-weight:300; } .modal-card p { margin:7px 0; color:var(--muted); font-size:15px; }
    .close { position:absolute; top:10px; right:12px; border:0; color:var(--muted); background:transparent; cursor:pointer; font-size:14px; } .close:hover,.close:focus-visible { color:var(--blue); text-decoration:underline; outline:0; }
    @media (max-width:480px) { header { height:60px; padding:0 20px; } main { padding:18px 16px 24px; } .search-area { transform:translateY(-7%); } .search-box { height:50px; } .search-label { display:none; } .description { font-size:15px; } footer { padding-inline:12px; } .nav-lines { gap:11px; } .nav-line { column-gap:25px; } .nav-link { font-size:13px; } .menu-card { bottom:145px; } }
  </style>
</head>
<body>
  <div class="page">
    <header><a class="logo" href="/" aria-label="Nuby, inicio">Nuby</a></header>
    <main>
      <section class="search-area" aria-label="Búsqueda en Nuby">
        <form class="search-box" id="search-form" role="search">
          <span class="search-label" aria-hidden="true">Buscar</span>
          <input id="query" name="q" type="search" placeholder="Buscar videojuegos, tecnología" autocomplete="off" aria-label="Buscar videojuegos y tecnología">
          <button class="microphone" type="button" id="voice" aria-label="Búsqueda por voz">Voz</button>
        </form>
        <p class="description">Tu espacio para descubrir videojuegos, tecnología y mucho más.</p>
      </section>
    </main>
    <footer>
      <nav class="nav-lines" aria-label="Navegación principal">
        <div class="nav-line"><button class="nav-link" data-home>Inicio</button><button class="nav-link" data-about>Acerca de Nuby</button><button class="nav-link" data-menu="Menú">Menú</button></div>
        <div class="nav-line"><button class="nav-link" data-menu="Configuración">Configuración</button><button class="nav-link" data-menu="Herramientas">Herramientas</button></div>
        <div class="nav-line"><button class="nav-link" data-menu="Historial">Historial</button><button class="nav-link" data-menu="Descargas">Descargas</button><button class="nav-link" data-menu="Ayuda">Ayuda</button></div>
      </nav>
    </footer>
  </div>
  <section class="menu-card" id="menu-card" aria-label="Submenú"><div class="menu-title" id="menu-title"></div><div id="menu-options"></div></section>
  <section class="modal" id="about-modal" aria-modal="true" role="dialog" aria-labelledby="about-title"><div class="modal-card"><button class="close" data-close>Cerrar</button><h2 id="about-title">Nuby</h2><p>Versión 2.4.1</p><p>Desarrollado con ❤️ para navegadores modernos</p></div></section>
  <script>
    (() => {
      const menus={"Configuración":["Tema: Claro / Oscuro / Automático","Idioma: Español / Inglés / Portugués","Privacidad: Limpiar datos / Cookies / Historial","Sincronización: Activar / Desactivar"],"Herramientas":["Inspeccionar elemento","Consola JavaScript","Red","Rendimiento"],"Historial":["Últimas 24 horas","Últimos 7 días","Últimas 4 semanas","Limpiar historial"],"Descargas":["Ver descargas","Carpeta de descargas","Limpiar lista"],"Ayuda":["Centro de ayuda","Reportar problema","Acerca de Nuby"],"Menú":["Nueva pestaña","Favoritos","Información de Nuby"]};
      const card=document.querySelector('#menu-card'), title=document.querySelector('#menu-title'), options=document.querySelector('#menu-options'), modal=document.querySelector('#about-modal');
      const closeMenu=()=>card.classList.remove('open');
      document.querySelectorAll('[data-menu]').forEach(button=>button.addEventListener('click',event=>{event.stopPropagation();const name=button.dataset.menu;title.textContent=name;options.replaceChildren(...menus[name].map(item=>{const b=document.createElement('button');b.className='menu-item';b.type='button';b.textContent=item;b.onclick=()=>{closeMenu();if(item==='Acerca de Nuby')modal.classList.add('open');else alert(item);};return b;}));card.classList.add('open');}));
      document.querySelector('[data-home]').onclick=()=>location.reload(); document.querySelector('[data-about]').onclick=()=>modal.classList.add('open'); document.querySelector('[data-close]').onclick=()=>modal.classList.remove('open'); modal.onclick=e=>{if(e.target===modal)modal.classList.remove('open');};
      document.addEventListener('click',e=>{if(!e.target.closest('.menu-card'))closeMenu();}); document.addEventListener('keydown',e=>{if(e.key==='Escape'){closeMenu();modal.classList.remove('open');}});
      document.querySelector('#voice').onclick=()=>alert('Búsqueda por voz'); document.querySelector('#search-form').onsubmit=e=>{e.preventDefault();const q=document.querySelector('#query').value.trim();if(q)location.href='https://www.google.com/search?q='+encodeURIComponent(q);};
    })();
  </script>
</body>
</html>
)NUBY_NATIVE";
}
} // namespace nuby::app
