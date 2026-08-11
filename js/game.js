// ============================================================
//  game.js — Bucle principal, cámara, render, entrada y estados.
// ============================================================

(() => {
  const canvas = document.getElementById("game");
  const ctx = canvas.getContext("2d");

  const titleScreen = document.getElementById("title-screen");
  const deathScreen = document.getElementById("death-screen");
  const winScreen  = document.getElementById("win-screen");
  const pauseScreen = document.getElementById("pause-screen");
  const hud        = document.getElementById("hud");
  const soundBtn   = document.getElementById("sound-btn");
  const touchControls = document.getElementById("touch-controls");

  let viewW = 0, viewH = 0, scale = 1;
  let lastTime = 0;
  let state = "title"; // title | playing | paused | dead | win
  let deathTimer = 0;

  const cam = { x: 0, y: 0, shake: 0 };
  let player, drones, checkpoints, particles;
  let activeCheckpoint = null;
  const START_X = 80, START_Y = 400;

  const input = {
    left: false, right: false, jump: false, jumpPressed: false,
  };

  // Eventos que otros módulos pueden disparar
  window.GameEvents = {
    onCheckpoint(cp) {
      activeCheckpoint = cp;
      Audio.checkpoint();
    }
  };

  // ---------- Configuración de tamaño ----------
  function resize() {
    viewW = window.innerWidth;
    viewH = window.innerHeight;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    canvas.width = Math.floor(viewW * dpr);
    canvas.height = Math.floor(viewH * dpr);
    canvas.style.width = viewW + "px";
    canvas.style.height = viewH + "px";
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    scale = Math.max(0.6, Math.min(1.4, viewW / 1280));

    let coarse = false;
    try { coarse = window.matchMedia && window.matchMedia("(pointer: coarse)").matches; } catch (e) {}
    if (coarse) touchControls.classList.remove("hidden");
  }
  window.addEventListener("resize", resize);

  // ---------- Inicialización del mundo ----------
  function buildWorld() {
    player = new Player(START_X, START_Y);
    drones = LEVEL.drones.map(d => new Drone(d));
    checkpoints = LEVEL.checkpoints.map(c => new Checkpoint(c.x, c.y));
    particles = new Particles(120, viewW, viewH);
    activeCheckpoint = null;
    cam.x = 0; cam.y = 0; cam.shake = 0;
  }

  function startGame() {
    buildWorld();
    state = "playing";
    titleScreen.classList.add("hidden");
    deathScreen.classList.add("hidden");
    winScreen.classList.add("hidden");
    pauseScreen.classList.add("hidden");
    hud.classList.remove("hidden");
    Audio.init();
    Audio.resume();
  }

  function respawnAtCheckpoint() {
    // Reaparecer en el checkpoint activo, reiniciando drones
    drones = LEVEL.drones.map(d => new Drone(d));
    checkpoints.forEach(c => { if (c !== activeCheckpoint) c.reset(); });
    const spawn = activeCheckpoint
      ? { x: activeCheckpoint.x - player.w / 2, y: activeCheckpoint.y + activeCheckpoint.h - player.h }
      : { x: START_X, y: START_Y };
    player.reset(spawn.x, spawn.y);
    state = "playing";
    deathScreen.classList.add("hidden");
    hud.classList.remove("hidden");
    cam.shake = 0;
  }

  function die() {
    if (state !== "playing") return;
    state = "dead";
    cam.shake = 18;
    deathTimer = 0;
    if (navigator.vibrate) navigator.vibrate([120, 60, 200]);
  }

  function win() {
    if (state !== "playing") return;
    state = "win";
    Audio.win();
    hud.classList.add("hidden");
    winScreen.classList.remove("hidden");
  }

  function togglePause() {
    if (state === "playing") {
      state = "paused";
      pauseScreen.classList.remove("hidden");
    } else if (state === "paused") {
      state = "playing";
      pauseScreen.classList.add("hidden");
    }
  }

  function toggleSound() {
    const muted = !Audio.isMuted();
    Audio.setMuted(muted);
    soundBtn.textContent = muted ? "🔇" : "🔊";
  }

  // ---------- Entrada ----------
  const keys = {};
  window.addEventListener("keydown", (e) => {
    if (["ArrowLeft","ArrowRight","ArrowUp","Space","KeyA","KeyD","KeyW","KeyP","Escape"].includes(e.code)) {
      e.preventDefault();
    }
    if (!keys[e.code]) {
      if (e.code === "Space" || e.code === "ArrowUp" || e.code === "KeyW") {
        input.jumpPressed = true;
      }
      if (e.code === "KeyP" || e.code === "Escape") togglePause();
    }
    keys[e.code] = true;
    Audio.resume();
  });
  window.addEventListener("keyup", (e) => { keys[e.code] = false; });

  function readKeyboard() {
    input.left = keys["ArrowLeft"] || keys["KeyA"];
    input.right = keys["ArrowRight"] || keys["KeyD"];
    input.jump = keys["Space"] || keys["ArrowUp"] || keys["KeyW"];
  }

  // Botones táctiles
  function bindTouch(id, prop) {
    const el = document.getElementById(id);
    const set = (v) => {
      input[prop] = v;
      if (prop === "jump" && v) input.jumpPressed = true;
      Audio.resume();
    };
    el.addEventListener("touchstart", (e) => { e.preventDefault(); set(true); }, { passive: false });
    el.addEventListener("touchend",   (e) => { e.preventDefault(); set(false); }, { passive: false });
    el.addEventListener("touchcancel",() => set(false));
    el.addEventListener("mousedown",  () => set(true));
    el.addEventListener("mouseup",    () => set(false));
    el.addEventListener("mouseleave", () => set(false));
  }
  bindTouch("btn-left", "left");
  bindTouch("btn-right", "right");
  bindTouch("btn-jump", "jump");

  document.getElementById("start-btn").addEventListener("click", startGame);
  document.getElementById("retry-btn").addEventListener("click", respawnAtCheckpoint);
  document.getElementById("win-btn").addEventListener("click", startGame);
  document.getElementById("resume-btn").addEventListener("click", togglePause);
  document.getElementById("restart-btn").addEventListener("click", startGame);
  document.getElementById("pause-btn").addEventListener("click", togglePause);
  soundBtn.addEventListener("click", toggleSound);

  // ---------- Cámara ----------
  function updateCamera(dt) {
    const targetX = player.x + player.w / 2 - viewW / 2;
    const targetY = player.y + player.h / 2 - viewH / 2 - 40;
    cam.x += (targetX - cam.x) * Math.min(1, dt * 6);
    cam.y += (targetY - cam.y) * Math.min(1, dt * 4);

    cam.x = Math.max(0, Math.min(LEVEL.WORLD_WIDTH - viewW, cam.x));
    cam.y = Math.max(-100, Math.min(LEVEL.WORLD_HEIGHT - viewH + 60, cam.y));

    if (cam.shake > 0) cam.shake = Math.max(0, cam.shake - dt * 40);
  }

  // ---------- Actualización ----------
  function update(dt) {
    if (state === "dead") {
      deathTimer += dt;
      if (deathTimer > 0.9) {
        hud.classList.add("hidden");
        deathScreen.classList.remove("hidden");
      }
      return;
    }
    if (state !== "playing") return;

    readKeyboard();
    player.update(dt, input, LEVEL.platforms);
    input.jumpPressed = false; // consumir

    for (const d of drones) d.update(dt, player);
    for (const c of checkpoints) c.update(dt, player);

    if (player.dead) { die(); return; }

    // meta
    const g = LEVEL.goal;
    if (player.x + player.w > g.x && player.x < g.x + g.w &&
        player.y + player.h > g.y && player.y < g.y + g.h) {
      win();
    }

    updateCamera(dt);
    particles.update(dt, cam);
  }

  // ---------- Render ----------
  function drawSky() {
    const grad = ctx.createLinearGradient(0, 0, 0, viewH);
    grad.addColorStop(0, "#0a0e1a");
    grad.addColorStop(0.6, "#070912");
    grad.addColorStop(1, "#020308");
    ctx.fillStyle = grad;
    ctx.fillRect(0, 0, viewW, viewH);

    const moonX = viewW * 0.78 - cam.x * 0.05;
    const moonY = viewH * 0.18;
    const mg = ctx.createRadialGradient(moonX, moonY, 0, moonX, moonY, 180);
    mg.addColorStop(0, "rgba(150,170,210,0.25)");
    mg.addColorStop(0.4, "rgba(90,110,150,0.08)");
    mg.addColorStop(1, "rgba(0,0,0,0)");
    ctx.fillStyle = mg;
    ctx.fillRect(moonX - 180, moonY - 180, 360, 360);
    ctx.fillStyle = "rgba(200,210,230,0.5)";
    ctx.beginPath();
    ctx.arc(moonX, moonY, 28, 0, Math.PI * 2);
    ctx.fill();
  }

  function drawBackdrop() {
    for (const layer of LEVEL.backdrop) {
      ctx.fillStyle = layer.color;
      const off = -cam.x * layer.factor;
      for (const [x, y, w, h] of layer.shapes) {
        ctx.fillRect(Math.round(x + off), y, w, h);
      }
    }
  }

  function drawLampPosts() {
    for (const lx of LEVEL.lampPosts) {
      const sx = Math.round(lx - cam.x * 0.85);
      if (sx < -100 || sx > viewW + 100) continue;
      const groundY = LEVEL.GROUND_Y - cam.y;

      ctx.fillStyle = "#05070d";
      ctx.fillRect(sx - 1, groundY - 120, 2, 120);

      const g = ctx.createRadialGradient(sx, groundY - 120, 0, sx, groundY - 120, 140);
      g.addColorStop(0, "rgba(180,200,240,0.18)");
      g.addColorStop(0.5, "rgba(120,140,190,0.05)");
      g.addColorStop(1, "rgba(0,0,0,0)");
      ctx.fillStyle = g;
      ctx.fillRect(sx - 140, groundY - 260, 280, 280);

      ctx.fillStyle = "rgba(220,230,255,0.8)";
      ctx.beginPath();
      ctx.arc(sx, groundY - 120, 3, 0, Math.PI * 2);
      ctx.fill();
    }
  }

  function drawPlatforms() {
    for (const p of LEVEL.platforms) {
      const sx = Math.round(p.x - cam.x);
      const sy = Math.round(p.y - cam.y);
      if (sx + p.w < -50 || sx > viewW + 50) continue;

      const grad = ctx.createLinearGradient(0, sy, 0, sy + p.h);
      grad.addColorStop(0, "#11151f");
      grad.addColorStop(0.05, "#0b0e16");
      grad.addColorStop(1, "#05060b");
      ctx.fillStyle = grad;
      ctx.fillRect(sx, sy, p.w, p.h);

      ctx.fillStyle = "rgba(90,110,150,0.25)";
      ctx.fillRect(sx, sy, p.w, 1);
    }
  }

  function drawGoal() {
    const g = LEVEL.goal;
    const sx = Math.round(g.x - cam.x);
    const sy = Math.round(g.y - cam.y);

    const grad = ctx.createRadialGradient(sx + g.w / 2, sy + g.h / 2, 0, sx + g.w / 2, sy + g.h / 2, 160);
    grad.addColorStop(0, "rgba(120,200,160,0.35)");
    grad.addColorStop(0.5, "rgba(80,150,120,0.1)");
    grad.addColorStop(1, "rgba(0,0,0,0)");
    ctx.fillStyle = grad;
    ctx.fillRect(sx - 130, sy - 130, g.w + 260, g.h + 260);

    ctx.fillStyle = "#0a120e";
    ctx.fillRect(sx, sy, g.w, g.h);
    ctx.strokeStyle = "rgba(120,220,170,0.6)";
    ctx.lineWidth = 2;
    ctx.strokeRect(sx, sy, g.w, g.h);

    ctx.fillStyle = "rgba(180,255,210,0.9)";
    ctx.font = "10px Arial";
    ctx.textAlign = "center";
    ctx.fillText("SALIDA", sx + g.w / 2, sy - 10);
  }

  function drawVignette() {
    const g = ctx.createRadialGradient(
      viewW / 2, viewH / 2, Math.min(viewW, viewH) * 0.3,
      viewW / 2, viewH / 2, Math.max(viewW, viewH) * 0.75
    );
    g.addColorStop(0, "rgba(0,0,0,0)");
    g.addColorStop(1, "rgba(0,0,0,0.75)");
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, viewW, viewH);
  }

  function render() {
    drawSky();

    ctx.save();
    if (cam.shake > 0) {
      ctx.translate(
        (Math.random() - 0.5) * cam.shake,
        (Math.random() - 0.5) * cam.shake
      );
    }

    drawBackdrop();
    drawLampPosts();
    drawPlatforms();

    if (checkpoints) for (const c of checkpoints) c.draw(ctx, cam);
    drawGoal();

    if (player) {
      for (const d of drones) d.draw(ctx, cam);
      player.draw(ctx, cam);
    }

    ctx.restore();

    particles.draw(ctx);
    drawVignette();
  }

  // ---------- Bucle ----------
  function loop(t) {
    if (!lastTime) lastTime = t;
    let dt = (t - lastTime) / 1000;
    lastTime = t;
    if (dt > 0.05) dt = 0.05;

    update(dt);
    render();
    requestAnimationFrame(loop);
  }

  // ---------- Arranque ----------
  resize();
  particles = new Particles(60, viewW, viewH);
  requestAnimationFrame(loop);
})();
