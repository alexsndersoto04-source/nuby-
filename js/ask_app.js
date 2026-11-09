/* Ask · Interfaz conversacional viva sobre arquitectura CORTEX-ML */
(function () {
  "use strict";

  const STORAGE_CFG = "ask_cortex_cfg_v1";
  const STORAGE_THREADS = "ask_cortex_threads_v1";

  const state = {
    cfg: {
      hfToken: "",
      hfUser: "",
      directUrl: "",
      resolvedUrl: "",
      voice: "femenina", // 'femenina' | 'masculina' | 'off'
    },
    online: false,
    forging: false,
    serverInfo: null,
    threads: [],
    activeThreadId: null,
    sending: false,
    speaking: false,
    currentAudio: null,
    recognition: null,
    listening: false,
  };

  // Puente HTTP asíncrono sin bloqueos ni errores CORS en Android
  const pendingHttp = new Map();
  let reqSeq = 1;

  window.__askHttpDone = function (reqId, b64Json) {
    const resolver = pendingHttp.get(reqId);
    if (!resolver) return;
    pendingHttp.delete(reqId);
    try {
      const bin = atob(b64Json);
      const bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
      const txt = new TextDecoder("utf-8").decode(bytes);
      resolver(JSON.parse(txt));
    } catch (err) {
      resolver({ ok: false, status: 0, error: String(err) });
    }
  };

  window.__askVoiceState = function (st) {
    setSpeakingIndicator(st === "speaking");
  };

  async function askHttp(method, url, bodyObj, authHeader) {
    const bodyStr = bodyObj ? JSON.stringify(bodyObj) : "";
    if (window.NubyNative && typeof window.NubyNative.httpRequestAsync === "function") {
      const id = "r" + (reqSeq++);
      return new Promise((resolve) => {
        pendingHttp.set(id, resolve);
        window.NubyNative.httpRequestAsync(id, method, url, bodyStr, authHeader || "");
        setTimeout(() => {
          if (pendingHttp.has(id)) {
            pendingHttp.delete(id);
            resolve({ ok: false, status: 0, error: "Tiempo de espera agotado" });
          }
        }, 98000);
      });
    }

    try {
      const headers = {};
      if (bodyObj) headers["Content-Type"] = "application/json";
      if (authHeader) headers["Authorization"] = authHeader;
      const res = await fetch(url, {
        method,
        headers,
        body: bodyObj ? bodyStr : undefined,
      });
      const ct = res.headers.get("content-type") || "";
      if (ct.includes("audio/")) {
        const buf = await res.arrayBuffer();
        let binary = "";
        const bytes = new Uint8Array(buf);
        for (let i = 0; i < bytes.byteLength; i++) binary += String.fromCharCode(bytes[i]);
        return { ok: res.ok, status: res.status, contentType: ct, body: btoa(binary) };
      }
      const text = await res.text();
      return { ok: res.ok, status: res.status, contentType: ct, body: text };
    } catch (err) {
      return { ok: false, status: 0, error: String(err) };
    }
  }

  // Persistencia local + respaldo privado en bóveda HF
  function loadLocalData() {
    try {
      const rawCfg = localStorage.getItem(STORAGE_CFG);
      if (rawCfg) Object.assign(state.cfg, JSON.parse(rawCfg));
    } catch (_) {}
    try {
      const rawT = localStorage.getItem(STORAGE_THREADS);
      if (rawT) state.threads = JSON.parse(rawT);
    } catch (_) {}

    if (!Array.isArray(state.threads) || state.threads.length === 0) {
      createNewThread(false);
    } else {
      state.activeThreadId = state.threads[0].id;
    }
  }

  function saveCfg() {
    try {
      localStorage.setItem(STORAGE_CFG, JSON.stringify(state.cfg));
    } catch (_) {}
  }

  function saveThreads() {
    try {
      localStorage.setItem(STORAGE_THREADS, JSON.stringify(state.threads));
    } catch (_) {}
    scheduleCloudDiaryBackup();
  }

  let backupTimer = null;
  function scheduleCloudDiaryBackup() {
    if (!state.cfg.hfToken || !state.cfg.hfUser) return;
    if (backupTimer) clearTimeout(backupTimer);
    backupTimer = setTimeout(syncDiaryToVault, 4500);
  }

  async function syncDiaryToVault() {
    if (!state.cfg.hfToken || !state.cfg.hfUser) return;
    const repo = `${state.cfg.hfUser}/cortex-macro-1m-backup`;
    const payload = {
      updatedAt: new Date().toISOString(),
      threads: state.threads.slice(0, 40),
    };
    const b64Content = btoa(unescape(encodeURIComponent(JSON.stringify(payload, null, 2))));
    const commitBody = {
      summary: "Actualizar diario privado de Ask",
      operations: [
        {
          key: "file",
          value: {
            content: b64Content,
            path: "DIARIO_ASK.json",
            encoding: "base64",
          },
        },
      ],
    };
    // Respaldo silencioso si el endpoint REST acepta JSON; nunca interrumpe al usuario
    try {
      await askHttp(
        "POST",
        `https://huggingface.co/api/datasets/${repo}/commit/main`,
        commitBody,
        `Bearer ${state.cfg.hfToken}`
      );
    } catch (_) {}
  }

  function activeThread() {
    let t = state.threads.find((x) => x.id === state.activeThreadId);
    if (!t) {
      t = createNewThread(false);
    }
    return t;
  }

  function createNewThread(renderNow = true) {
    const t = {
      id: "th_" + Date.now() + "_" + Math.random().toString(36).slice(2, 6),
      title: "Nueva conversación",
      updatedAt: Date.now(),
      messages: [],
    };
    state.threads.unshift(t);
    state.activeThreadId = t.id;
    saveThreads();
    if (renderNow) {
      renderThreadsList();
      renderActiveThread();
      // Reiniciar contexto conversacional en el servidor si está activo
      const base = currentBaseUrl();
      if (base) askHttp("POST", `${base}/api/chat`, { mensaje: "/nuevo" });
    }
    return t;
  }

  function currentBaseUrl() {
    const raw = (state.cfg.directUrl || state.cfg.resolvedUrl || "").trim().replace(/\/+$/, "");
    if (raw) return raw;
    if (location.protocol.startsWith("http") && location.hostname !== "localhost") {
      return location.origin;
    }
    return "";
  }

  // Descubrimiento automático del túnel vivo desde la bóveda privada Hugging Face
  async function discoverCloudUrl() {
    const token = (state.cfg.hfToken || "").trim();
    if (!token && !state.cfg.directUrl) {
      updateConnectionBadge(false, false, "Toca arriba para vincular tu nube");
      return false;
    }

    if (token) {
      const who = await askHttp("GET", "https://huggingface.co/api/whoami-v2", null, `Bearer ${token}`);
      if (who.ok && who.body) {
        try {
          const info = JSON.parse(who.body);
          if (info && info.name) {
            state.cfg.hfUser = info.name;
            saveCfg();
          }
        } catch (_) {}
      }

      if (state.cfg.hfUser) {
        const repo = `${state.cfg.hfUser}/cortex-macro-1m-backup`;
        const linkRes = await askHttp(
          "GET",
          `https://huggingface.co/datasets/${repo}/resolve/main/ENLACE_VIVO.txt?t=${Date.now()}`,
          null,
          `Bearer ${token}`
        );
        if (linkRes.ok && linkRes.body) {
          const m = linkRes.body.match(/https:\/\/[a-zA-Z0-9-]+\.trycloudflare\.com/);
          if (m && m[0]) {
            state.cfg.resolvedUrl = m[0];
            saveCfg();
          }
        }
      }
    }

    return await checkBrainHealth();
  }

  async function checkBrainHealth() {
    const base = currentBaseUrl();
    if (!base) {
      updateConnectionBadge(false, Boolean(state.cfg.hfToken), state.cfg.hfToken ? "Esperando servidor 16 GB..." : "Vincular con tu nube");
      return false;
    }

    const res = await askHttp("GET", `${base}/api/estado`);
    if (!res.ok || !res.body) {
      // Si había token y el túnel rotó tras 5h, reintentar descubrir desde HF
      state.online = false;
      updateConnectionBadge(false, true, "Reconectando con la nube...");
      return false;
    }

    try {
      const d = JSON.parse(res.body);
      state.serverInfo = d;
      const mb = ((d.bytes_asimilados || 0) / 1048576).toFixed(1);
      if (d.listo_chat) {
        state.online = true;
        state.forging = false;
        updateConnectionBadge(true, false, `CORTEX-ML · ${mb} MB asimilados`);
      } else {
        state.online = false;
        state.forging = true;
        updateConnectionBadge(false, true, `CORTEX-ML · ${d.fase_actual || "forjando 1M neuronas"}`);
      }
      const tel = document.getElementById("brainTelemetry");
      if (tel) {
        const n = Number(d.neuronas || 1017856).toLocaleString();
        tel.textContent = `${n} neuronas LIF · ${d.lotes_completados || 0} lotes (${mb} MB)`;
      }
      return Boolean(d.listo_chat);
    } catch (_) {
      return false;
    }
  }

  function updateConnectionBadge(online, forging, text) {
    const dot = document.getElementById("connDot");
    const sub = document.getElementById("connSub");
    if (dot) {
      dot.className = "pulse-dot" + (online ? " online" : forging ? " forging" : "");
    }
    if (sub) sub.textContent = text;
  }

  // Síntesis de voz humana natural (Femenina cálida / Masculina cálida)
  function stopAllVoice() {
    if (state.currentAudio) {
      try {
        state.currentAudio.pause();
        state.currentAudio = null;
      } catch (_) {}
    }
    if (window.NubyNative && typeof window.NubyNative.stopSpeaking === "function") {
      window.NubyNative.stopSpeaking();
    }
    if (window.speechSynthesis) {
      try {
        window.speechSynthesis.cancel();
      } catch (_) {}
    }
    setSpeakingIndicator(false);
  }

  function setSpeakingIndicator(active) {
    state.speaking = active;
    const chip = document.getElementById("btnVoiceToggle");
    if (chip) chip.classList.toggle("speaking", active);
  }

  async function speakNatural(text, forceGender) {
    const gender = forceGender || state.cfg.voice;
    if (gender === "off" && !forceGender) return;
    const clean = String(text || "")
      .replace(/«[\s\S]*?»/g, " ")
      .replace(/\s+/g, " ")
      .trim();
    if (!clean) return;

    stopAllVoice();
    setSpeakingIndicator(true);

    // 1. Intentar voz neuronal de estudio desde el servidor CORTEX-ML (/api/voz)
    const base = currentBaseUrl();
    if (base) {
      const ttsRes = await askHttp("POST", `${base}/api/voz`, { texto: clean, voz: gender });
      if (ttsRes.ok && ttsRes.body && (ttsRes.contentType || "").includes("audio/")) {
        try {
          const audio = new Audio("data:audio/mpeg;base64," + ttsRes.body);
          state.currentAudio = audio;
          audio.onended = () => setSpeakingIndicator(false);
          audio.onerror = () => setSpeakingIndicator(false);
          await audio.play();
          return;
        } catch (_) {}
      }
    }

    // 2. Motor neuronal HD nativo de Android (selecciona voces en red de alta calidad humana)
    if (window.NubyNative && typeof window.NubyNative.speakNeural === "function") {
      const ok = window.NubyNative.speakNeural(clean, gender);
      if (ok) return;
    }

    // 3. Respaldo Web Speech en escritorio
    if (window.speechSynthesis) {
      const u = new SpeechSynthesisUtterance(clean);
      u.lang = "es-ES";
      u.rate = 0.97;
      u.pitch = gender === "masculina" ? 0.94 : 1.03;
      const voices = window.speechSynthesis.getVoices().filter((v) => v.lang && v.lang.toLowerCase().startsWith("es"));
      if (voices.length) {
        const wantMale = gender === "masculina";
        const match = voices.find((v) => {
          const n = v.name.toLowerCase();
          return wantMale
            ? n.includes("jorge") || n.includes("alvaro") || n.includes("gonzalo") || n.includes("tomas") || n.includes("male")
            : n.includes("dalia") || n.includes("salome") || n.includes("elvira") || n.includes("paulina") || n.includes("monica");
        });
        u.voice = match || voices[0];
      }
      u.onend = () => setSpeakingIndicator(false);
      u.onerror = () => setSpeakingIndicator(false);
      window.speechSynthesis.speak(u);
      return;
    }

    setSpeakingIndicator(false);
  }

  // Renderizado limpio y orgánico de mensajes + pensamiento cortical
  function renderThreadsList() {
    const box = document.getElementById("threadsList");
    if (!box) return;
    box.innerHTML = "";
    state.threads.forEach((t) => {
      const btn = document.createElement("button");
      btn.className = "thread-item" + (t.id === state.activeThreadId ? " active" : "");
      btn.textContent = t.title || "Conversación";
      btn.onclick = () => {
        state.activeThreadId = t.id;
        renderThreadsList();
        renderActiveThread();
        toggleDrawer(false);
      };
      box.appendChild(btn);
    });
  }

  function renderActiveThread() {
    const t = activeThread();
    const hero = document.getElementById("welcomeHero");
    const stream = document.getElementById("messagesStream");
    if (!stream) return;

    stream.innerHTML = "";
    if (!t.messages.length) {
      if (hero) hero.style.display = "flex";
      return;
    }
    if (hero) hero.style.display = "none";

    t.messages.forEach((m) => appendMessageDom(m, false));
    scrollToBottom();
  }

  function formatThoughtSummary(meta) {
    if (!meta) return "Pensamiento cortical";
    const parts = ["Pensamiento cortical"];
    if (Array.isArray(meta.columnas_trn) && meta.columnas_trn.length) {
      parts.push(`${meta.columnas_trn.length} columnas TRN`);
    }
    if (typeof meta.certeza === "number" && meta.certeza > 0) {
      parts.push(`certeza ${Math.round(meta.certeza * 100)}%`);
    }
    if (meta.aprendio_vivo) {
      parts.push("recordado en memoria viva");
    }
    return parts.join(" · ");
  }

  function appendMessageDom(msg, animateScroll = true) {
    const hero = document.getElementById("welcomeHero");
    if (hero) hero.style.display = "none";
    const stream = document.getElementById("messagesStream");
    if (!stream) return null;

    if (msg.role === "user") {
      const div = document.createElement("div");
      div.className = "turn-user";
      div.textContent = msg.text;
      stream.appendChild(div);
      if (animateScroll) scrollToBottom();
      return div;
    }

    const wrap = document.createElement("div");
    wrap.className = "turn-ask";

    const meta = msg.meta || {};
    const hasThought = Boolean(
      meta.thinkingNow ||
      meta.pensamiento ||
      (Array.isArray(meta.columnas_trn) && meta.columnas_trn.length) ||
      meta.aprendio_vivo ||
      meta.abrio_go === false
    );

    if (hasThought) {
      const cap = document.createElement("div");
      cap.className = "thought-capsule";

      const head = document.createElement("div");
      head.className = "thought-header";

      const dot = document.createElement("span");
      dot.className = "thought-dot" + (meta.thinkingNow ? " thinking" : "");
      head.appendChild(dot);

      const title = document.createElement("span");
      title.textContent = meta.thinkingNow
        ? "Deliberando en columnas corticales..."
        : formatThoughtSummary(meta);
      head.appendChild(title);
      cap.appendChild(head);

      if (!meta.thinkingNow) {
        const body = document.createElement("div");
        body.className = "thought-body";
        const innerText = document.createElement("div");
        innerText.textContent = meta.pensamiento
          ? meta.pensamiento
          : meta.abrio_go === false
          ? "Compuerta de Ganglios Basales (No-Go): la corteza prefirió abstenerse con honestidad antes de inventar un dato incierto."
          : "Activación directa por resonancia talamocortical.";
        body.appendChild(innerText);

        const pills = document.createElement("div");
        pills.className = "thought-pills";
        if (typeof meta.ach === "number") {
          const p = document.createElement("span");
          p.className = "micro-pill";
          p.textContent = `Sorpresa ACh ${(meta.ach * 100).toFixed(0)}%`;
          pills.appendChild(p);
        }
        if (Array.isArray(meta.columnas_trn)) {
          meta.columnas_trn.slice(0, 4).forEach((c) => {
            const p = document.createElement("span");
            p.className = "micro-pill";
            p.textContent = `Columna #${c[0]}`;
            pills.appendChild(p);
          });
        }
        body.appendChild(pills);
        cap.appendChild(body);
        cap.onclick = () => cap.classList.toggle("open");
      }

      wrap.appendChild(cap);
    }

    if (!meta.thinkingNow) {
      const prose = document.createElement("div");
      prose.className = "ask-prose";
      prose.textContent = msg.text || "";
      wrap.appendChild(prose);

      const acts = document.createElement("div");
      acts.className = "ask-actions";

      const btnListen = document.createElement("button");
      btnListen.className = "tiny-btn";
      btnListen.innerHTML =
        '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"></polygon><path d="M15.5 8.5a5 5 0 0 1 0 7"></path></svg><span>Escuchar</span>';
      btnListen.onclick = () => speakNatural(msg.text, state.cfg.voice === "off" ? "femenina" : state.cfg.voice);
      acts.appendChild(btnListen);

      wrap.appendChild(acts);
    }

    stream.appendChild(wrap);
    if (animateScroll) scrollToBottom();
    return wrap;
  }

  function scrollToBottom() {
    const canvas = document.getElementById("chatCanvas");
    if (canvas) {
      requestAnimationFrame(() => {
        canvas.scrollTop = canvas.scrollHeight;
      });
    }
  }

  // Envío de mensaje conversacional a Ask en la nube
  async function sendMessage(rawText) {
    const text = String(rawText || "").trim();
    if (!text || state.sending) return;

    state.sending = true;
    const input = document.getElementById("msgInput");
    if (input) {
      input.value = "";
      input.style.height = "auto";
    }
    updateSendButtonState();

    const thread = activeThread();
    if (thread.messages.length === 0) {
      thread.title = text.slice(0, 42);
      renderThreadsList();
    }

    const userMsg = { role: "user", text, ts: Date.now() };
    thread.messages.push(userMsg);
    thread.updatedAt = Date.now();
    saveThreads();
    appendMessageDom(userMsg, true);

    const placeholderDom = appendMessageDom(
      { role: "ask", text: "", meta: { thinkingNow: true } },
      true
    );

    let base = currentBaseUrl();
    if (!base) {
      await discoverCloudUrl();
      base = currentBaseUrl();
    }

    let replyText = "";
    let meta = {};

    if (!base) {
      replyText = !state.cfg.hfToken
        ? "Para conectarme con mi corteza de 1 millón de neuronas en la nube, toca el botón de arriba a la derecha y pega una sola vez tu llave privada (hf_...)."
        : "Mi corteza en la nube de 16 GB RAM está terminando de inicializarse y publicará su enlace vivo en unos instantes. Intenta enviarme tu mensaje de nuevo en un par de minutos.";
    } else {
      let res = await askHttp("POST", `${base}/api/chat`, { mensaje: text });
      // Si el túnel de 5h rotó, redescubrir automáticamente desde la bóveda HF y reintentar 1 vez
      if (!res.ok && state.cfg.hfToken) {
        await discoverCloudUrl();
        base = currentBaseUrl();
        if (base) res = await askHttp("POST", `${base}/api/chat`, { mensaje: text });
      }

      if (res.ok && res.body) {
        try {
          const d = JSON.parse(res.body);
          replyText = d.respuesta || "Estoy procesando ese concepto en mi corteza.";
          meta = {
            pensamiento: d.pensamiento || "",
            ach: d.ach,
            certeza: d.certeza,
            abrio_go: d.abrio_go,
            aprendio_vivo: d.aprendio_vivo,
            columnas_trn: d.columnas_trn || [],
          };
        } catch (_) {
          replyText = "Recibí una señal incompleta desde la nube. Intenta de nuevo.";
        }
      } else {
        replyText = "Aún estoy reconectando con el servidor de 16 GB RAM en la nube. Verifica que el flujo 'Corteza Viva Macro 1M' esté en ejecución.";
      }
    }

    if (placeholderDom && placeholderDom.parentNode) {
      placeholderDom.parentNode.removeChild(placeholderDom);
    }

    const askMsg = { role: "ask", text: replyText, meta, ts: Date.now() };
    thread.messages.push(askMsg);
    thread.updatedAt = Date.now();
    saveThreads();
    appendMessageDom(askMsg, true);

    state.sending = false;
    if (state.cfg.voice !== "off" && replyText) {
      speakNatural(replyText);
    }
  }

  // Dictado por micrófono manos libres
  function toggleMicDictation() {
    const SR = window.SpeechRecognition || window.webkitSpeechRecognition;
    const btn = document.getElementById("btnMic");

    if (window.NubyNative && typeof window.NubyNative.hasMicrophone === "function") {
      if (!window.NubyNative.hasMicrophone()) {
        window.NubyNative.requestMicrophone();
        return;
      }
    }

    if (!SR) {
      const inp = document.getElementById("msgInput");
      if (inp) inp.focus();
      return;
    }

    if (state.listening && state.recognition) {
      state.recognition.stop();
      return;
    }

    stopAllVoice();
    const rec = new SR();
    rec.lang = "es-ES";
    rec.interimResults = true;
    rec.continuous = false;
    state.recognition = rec;

    rec.onstart = () => {
      state.listening = true;
      if (btn) btn.classList.add("listening");
    };
    rec.onresult = (ev) => {
      let transcript = "";
      for (let i = 0; i < ev.results.length; i++) {
        transcript += ev.results[i][0].transcript;
      }
      const inp = document.getElementById("msgInput");
      if (inp) {
        inp.value = transcript;
        autoGrowInput();
        updateSendButtonState();
      }
      if (ev.results[ev.results.length - 1].isFinal && transcript.trim()) {
        setTimeout(() => sendMessage(transcript.trim()), 280);
      }
    };
    rec.onerror = () => {
      state.listening = false;
      if (btn) btn.classList.remove("listening");
    };
    rec.onend = () => {
      state.listening = false;
      if (btn) btn.classList.remove("listening");
    };
    rec.start();
  }

  // Controles de interfaz
  function toggleDrawer(open) {
    const dr = document.getElementById("historyDrawer");
    const bd = document.getElementById("drawerBackdrop");
    if (!dr || !bd) return;
    dr.classList.toggle("open", open);
    bd.hidden = !open;
  }

  function refreshVoiceUi() {
    const lbl = document.getElementById("voiceLabel");
    if (lbl) {
      lbl.textContent =
        state.cfg.voice === "femenina"
          ? "Ella"
          : state.cfg.voice === "masculina"
          ? "Él"
          : "Mudo";
    }
    document.querySelectorAll("#voiceSelector button").forEach((b) => {
      b.classList.toggle("active", b.dataset.voice === state.cfg.voice);
    });
  }

  function autoGrowInput() {
    const inp = document.getElementById("msgInput");
    if (!inp) return;
    inp.style.height = "auto";
    inp.style.height = Math.min(132, inp.scrollHeight) + "px";
  }

  function updateSendButtonState() {
    const inp = document.getElementById("msgInput");
    const btn = document.getElementById("btnSend");
    if (!inp || !btn) return;
    btn.classList.toggle("ready", inp.value.trim().length > 0 && !state.sending);
  }

  function bindEvents() {
    const inp = document.getElementById("msgInput");
    const btnSend = document.getElementById("btnSend");
    const btnMic = document.getElementById("btnMic");

    inp.addEventListener("input", () => {
      autoGrowInput();
      updateSendButtonState();
    });

    inp.addEventListener("keydown", (e) => {
      if (e.key === "Enter" && !e.shiftKey && window.innerWidth > 700) {
        e.preventDefault();
        sendMessage(inp.value);
      }
    });

    btnSend.addEventListener("click", () => sendMessage(inp.value));
    btnMic.addEventListener("click", toggleMicDictation);

    document.querySelectorAll(".soft-chip").forEach((chip) => {
      chip.addEventListener("click", () => sendMessage(chip.dataset.prompt || ""));
    });

    document.getElementById("btnHistory").addEventListener("click", () => toggleDrawer(true));
    document.getElementById("drawerBackdrop").addEventListener("click", () => toggleDrawer(false));
    document.getElementById("btnNewChat").addEventListener("click", () => {
      createNewThread(true);
      toggleDrawer(false);
    });

    document.getElementById("btnVoiceToggle").addEventListener("click", () => {
      if (state.speaking) {
        stopAllVoice();
        return;
      }
      state.cfg.voice =
        state.cfg.voice === "femenina"
          ? "masculina"
          : state.cfg.voice === "masculina"
          ? "off"
          : "femenina";
      saveCfg();
      refreshVoiceUi();
    });

    const dlg = document.getElementById("settingsDialog");
    const openSettings = () => {
      document.getElementById("hfTokenInput").value = state.cfg.hfToken || "";
      document.getElementById("directUrlInput").value = state.cfg.directUrl || "";
      refreshVoiceUi();
      dlg.showModal();
    };

    document.getElementById("btnSettings").addEventListener("click", openSettings);
    document.getElementById("btnStatusOpen").addEventListener("click", openSettings);
    document.getElementById("btnCloseSettings").addEventListener("click", () => dlg.close());

    document.querySelectorAll("#voiceSelector button").forEach((b) => {
      b.addEventListener("click", () => {
        state.cfg.voice = b.dataset.voice;
        saveCfg();
        refreshVoiceUi();
      });
    });

    document.getElementById("btnTestVoice").addEventListener("click", () => {
      const g = state.cfg.voice === "off" ? "femenina" : state.cfg.voice;
      speakNatural("Hola, soy Ask. Estoy conectado a mi arquitectura viva CORTEX-ML.", g);
    });

    document.getElementById("btnSaveSettings").addEventListener("click", async () => {
      state.cfg.hfToken = document.getElementById("hfTokenInput").value.trim();
      state.cfg.directUrl = document.getElementById("directUrlInput").value.trim();
      saveCfg();
      dlg.close();
      await discoverCloudUrl();
    });

    document.getElementById("btnTeachNow").addEventListener("click", async () => {
      const box = document.getElementById("teachInput");
      const txt = box.value.trim().replace(/\s+/g, " ");
      if (!txt) return;
      box.value = "";
      dlg.close();
      await sendMessage("/asimilar " + txt);
    });

    document.getElementById("btnDream").addEventListener("click", async () => {
      toggleDrawer(false);
      await sendMessage("/sonar");
    });
  }

  // Arranque
  loadLocalData();
  refreshVoiceUi();
  renderThreadsList();
  renderActiveThread();
  bindEvents();
  discoverCloudUrl();
  setInterval(checkBrainHealth, 25000);
})();
