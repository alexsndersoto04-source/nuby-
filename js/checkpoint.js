// ============================================================
//  checkpoint.js — Puntos de control (faroles).
//  Al tocarlos se activan; al morir reapareces en el último activo.
// ============================================================

class Checkpoint {
  constructor(x, y) {
    this.x = x;        // posición central
    this.y = y;        // parte superior del farol
    this.w = 18;
    this.h = 54;
    this.active = false;
    this.flicker = Math.random() * Math.PI * 2;
  }

  reset() { this.active = false; }

  update(dt, player) {
    this.flicker += dt * 8;
    if (this.active) return;

    // ¿el jugador lo toca?
    const px = player.x + player.w / 2;
    if (px > this.x - 24 && px < this.x + 24 &&
        player.y + player.h > this.y && player.y < this.y + this.h) {
      this.active = true;
      if (window.GameEvents) GameEvents.onCheckpoint(this);
    }
  }

  draw(ctx, cam) {
    const sx = Math.round(this.x - cam.x);
    const sy = Math.round(this.y - cam.y);
    if (sx < -80 || sx > ctx.canvas.width + 80) return;

    const baseColor = this.active ? [255, 190, 90] : [90, 110, 150];
    const glow = this.active ? (0.7 + Math.sin(this.flicker) * 0.15) : 0.25;

    // halo
    if (this.active) {
      const r = 90;
      const g = ctx.createRadialGradient(sx, sy + 14, 0, sx, sy + 14, r);
      g.addColorStop(0, `rgba(${baseColor[0]},${baseColor[1]},${baseColor[2]},${0.35 * glow})`);
      g.addColorStop(1, "rgba(0,0,0,0)");
      ctx.fillStyle = g;
      ctx.fillRect(sx - r, sy + 14 - r, r * 2, r * 2);
    }

    // poste
    ctx.fillStyle = "#05070d";
    ctx.fillRect(sx - 1, sy + 14, 2, this.h - 14);

    // farol
    ctx.fillStyle = this.active
      ? `rgba(${baseColor[0]},${baseColor[1]},${baseColor[2]},${0.9})`
      : "rgba(120,140,180,0.35)";
    ctx.fillRect(sx - 5, sy + 2, 10, 14);
    ctx.strokeStyle = this.active
      ? `rgba(255,220,150,0.8)`
      : "rgba(90,110,150,0.5)";
    ctx.lineWidth = 1;
    ctx.strokeRect(sx - 5, sy + 2, 10, 14);
  }
}
