// ============================================================
//  drone.js — Dron enemigo: patrulla y barre con un cono de luz.
//  Si el jugador entra en el haz y no está cubierto, lo detecta.
// ============================================================

class Drone {
  constructor(cfg) {
    this.x = cfg.x;
    this.y = cfg.y;
    this.x1 = cfg.x1;
    this.x2 = cfg.x2;
    this.coneLen = cfg.cone;
    this.speed = cfg.speed;
    this.dir = 1;
    this.w = 34;
    this.h = 16;
    this.alert = 0;          // 0..1 : nivel de alerta
    this.alertCooldown = 0;
    this.beamPulse = 0;
    this.bob = Math.random() * Math.PI * 2;
  }

  update(dt, player) {
    // patrulla horizontal
    this.x += this.dir * this.speed * dt;
    if (this.x < this.x1) { this.x = this.x1; this.dir = 1; }
    if (this.x + this.w > this.x2) { this.x = this.x2 - this.w; this.dir = -1; }

    this.bob += dt * 2;
    this.beamPulse += dt * 4;

    // detección
    const detected = this.canSee(player) && !player.dead;

    if (detected) {
      this.alert = Math.min(1, this.alert + dt * 0.9);
      if (this.alert > 0.2 && this.alertCooldown <= 0) {
        Audio.droneAlert(this.alert);
        this.alertCooldown = 0.35;
      }
      if (this.alert >= 1) {
        player.die();
      }
    } else {
      this.alert = Math.max(0, this.alert - dt * 0.6);
    }
    this.alertCooldown -= dt;
  }

  // El haz es un cono triangular que apunta hacia abajo.
  // Comprobamos si el jugador (su caja) está dentro.
  canSee(player) {
    const tipX = this.x + this.w / 2;
    const tipY = this.y + this.h;
    const bottomY = tipY + this.coneLen;
    const halfBase = this.coneLen * 0.42; // anchura del cono abajo

    // ¿caja del jugador dentro del rango vertical del cono?
    const px2 = player.x + player.w;
    const py2 = player.y + player.h;
    if (py2 < tipY || player.y > bottomY) return false;

    // interpola la anchura del cono a la altura del jugador
    const closestY = Math.max(tipY, Math.min(bottomY, player.y + player.h / 2));
    const t = (closestY - tipY) / this.coneLen;
    const centerX = tipX;
    const halfW = 4 + halfBase * t;
    const left = centerX - halfW;
    const right = centerX + halfW;

    return px2 > left && player.x < right;
  }

  draw(ctx, cam) {
    const sx = Math.round(this.x - cam.x);
    const sy = Math.round(this.y - cam.y + Math.sin(this.bob) * 2);

    // --- Cono de luz ---
    const tipX = sx + this.w / 2;
    const tipY = sy + this.h;
    const bottomY = tipY + this.coneLen;
    const halfBase = this.coneLen * 0.42;
    const pulse = 0.85 + Math.sin(this.beamPulse) * 0.06;

    // color según alerta
    const r = Math.round(180 + 75 * this.alert);
    const g = Math.round(200 - 150 * this.alert);
    const b = Math.round(230 - 180 * this.alert);
    const beamAlpha = (0.10 + 0.18 * this.alert) * pulse;

    const grad = ctx.createLinearGradient(0, tipY, 0, bottomY);
    grad.addColorStop(0, `rgba(${r},${g},${b},${beamAlpha + 0.12})`);
    grad.addColorStop(1, `rgba(${r},${g},${b},0)`);

    ctx.save();
    ctx.beginPath();
    ctx.moveTo(tipX, tipY);
    ctx.lineTo(tipX - halfBase, bottomY);
    ctx.lineTo(tipX + halfBase, bottomY);
    ctx.closePath();
    ctx.fillStyle = grad;
    ctx.fill();

    // borde del cono
    ctx.strokeStyle = `rgba(${r},${g},${b},${0.25 + 0.4 * this.alert})`;
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.restore();

    // --- Cuerpo del dron ---
    ctx.save();
    ctx.fillStyle = "#0a0d16";
    ctx.strokeStyle = this.alert > 0.3
      ? `rgba(255,80,60,${0.6 + this.alert * 0.4})`
      : "rgba(120,150,200,0.5)";
    ctx.lineWidth = 1.5;

    // cuerpo principal
    ctx.beginPath();
    ctx.ellipse(tipX, sy + this.h / 2, this.w / 2, this.h / 2, 0, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();

    // hélice
    ctx.strokeStyle = "rgba(150,170,210,0.35)";
    ctx.beginPath();
    ctx.moveTo(sx - 4, sy + 2);
    ctx.lineTo(sx + this.w + 4, sy + 2);
    ctx.stroke();

    // ojo / lente
    ctx.fillStyle = `rgba(${r},${g},${b},${0.7 + this.alert * 0.3})`;
    ctx.beginPath();
    ctx.arc(tipX, sy + this.h / 2 + 2, 3, 0, Math.PI * 2);
    ctx.fill();

    ctx.restore();
  }
}
