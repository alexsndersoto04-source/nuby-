// ============================================================
//  particles.js — Partículas atmosféricas: polvo flotante y
//  destellos. Añaden profundidad y vida al aire.
// ============================================================

class Particles {
  constructor(count, viewW, viewH) {
    this.particles = [];
    for (let i = 0; i < count; i++) {
      this.particles.push({
        x: Math.random() * viewW,
        y: Math.random() * viewH,
        r: 0.4 + Math.random() * 1.6,
        vy: 3 + Math.random() * 8,
        vx: (Math.random() - 0.5) * 6,
        alpha: 0.1 + Math.random() * 0.4,
        twinkle: Math.random() * Math.PI * 2,
      });
    }
    this.viewW = viewW;
    this.viewH = viewH;
  }

  update(dt, cam) {
    for (const p of this.particles) {
      p.y += p.vy * dt;
      p.x += p.vx * dt + Math.sin(p.twinkle) * 0.3;
      p.twinkle += dt * 2;
      if (p.y > this.viewH + 10) {
        p.y = -10;
        p.x = Math.random() * this.viewW;
      }
      if (p.x > this.viewW + 10) p.x = -10;
      if (p.x < -10) p.x = this.viewW + 10;
    }
  }

  draw(ctx) {
    ctx.save();
    for (const p of this.particles) {
      const a = p.alpha * (0.6 + 0.4 * Math.sin(p.twinkle));
      ctx.fillStyle = `rgba(180,200,230,${a})`;
      ctx.beginPath();
      ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.restore();
  }
}
