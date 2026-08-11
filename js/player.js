// ============================================================
//  player.js — El niño protagonista (silueta).
//  Física de plataformas: mover, saltar, gravedad, colisiones AABB.
// ============================================================

class Player {
  constructor(x, y) {
    this.x = x;
    this.y = y;
    this.w = 22;
    this.h = 46;
    this.vx = 0;
    this.vy = 0;
    this.onGround = false;
    this.facing = 1;
    this.speed = 230;
    this.jumpForce = 520;
    this.gravity = 1500;
    this.maxFall = 900;

    // animación
    this.animTime = 0;
    this.stepTimer = 0;
    this.dead = false;
  }

  reset(x, y) {
    this.x = x; this.y = y;
    this.vx = 0; this.vy = 0;
    this.dead = false;
    this.onGround = false;
  }

  update(dt, input, platforms) {
    if (this.dead) return;

    // --- Entrada horizontal ---
    let move = 0;
    if (input.left) move -= 1;
    if (input.right) move += 1;

    const targetVx = move * this.speed;
    // aceleración suave
    const accel = this.onGround ? 1800 : 1100;
    if (this.vx < targetVx) this.vx = Math.min(targetVx, this.vx + accel * dt);
    else if (this.vx > targetVx) this.vx = Math.max(targetVx, this.vx - accel * dt);

    if (move !== 0) this.facing = move;

    // --- Salto ---
    if (input.jumpPressed && this.onGround) {
      this.vy = -this.jumpForce;
      this.onGround = false;
      Audio.jump();
    }
    // salto variable: si sueltas pronto, corta el impulso
    if (!input.jump && this.vy < -180) this.vy = -180;

    // --- Gravedad ---
    const wasOnGround = this.onGround;
    this.vy = Math.min(this.vy + this.gravity * dt, this.maxFall);

    // --- Mover + colisionar por ejes ---
    this.x += this.vx * dt;
    this.resolveCollisions(platforms, "x");

    this.y += this.vy * dt;
    this.onGround = false;
    this.resolveCollisions(platforms, "y");

    // límites del mundo
    if (this.x < 0) { this.x = 0; this.vx = 0; }
    if (this.x + this.w > LEVEL.WORLD_WIDTH) {
      this.x = LEVEL.WORLD_WIDTH - this.w; this.vx = 0;
    }
    // caer al vacío = muerte
    if (this.y > LEVEL.WORLD_HEIGHT + 100) {
      this.die();
    }

    // sonido de aterrizaje
    if (!wasOnGround && this.onGround && this.vy === 0) {
      Audio.land();
    }

    // pasos
    if (this.onGround && Math.abs(this.vx) > 30) {
      this.stepTimer -= dt;
      if (this.stepTimer <= 0) {
        Audio.footstep();
        this.stepTimer = 0.32;
      }
      this.animTime += dt * Math.abs(this.vx) * 0.04;
    } else {
      this.stepTimer = 0;
      this.animTime = 0;
    }
  }

  // Colisión AABB contra plataformas, resuelta en un eje a la vez
  resolveCollisions(platforms, axis) {
    for (const p of platforms) {
      if (!this.overlaps(p)) continue;

      if (axis === "x") {
        if (this.vx > 0) this.x = p.x - this.w;
        else if (this.vx < 0) this.x = p.x + p.w;
        this.vx = 0;
      } else {
        if (this.vy > 0) {
          this.y = p.y - this.h;
          this.vy = 0;
          this.onGround = true;
        } else if (this.vy < 0) {
          this.y = p.y + p.h;
          this.vy = 0;
        }
      }
    }
  }

  overlaps(r) {
    return this.x < r.x + r.w &&
           this.x + this.w > r.x &&
           this.y < r.y + r.h &&
           this.y + this.h > r.y;
  }

  die() {
    if (this.dead) return;
    this.dead = true;
    Audio.death();
  }

  draw(ctx, cam) {
    const sx = Math.round(this.x - cam.x);
    const sy = Math.round(this.y - cam.y);

    // sombra en el suelo
    ctx.save();
    ctx.fillStyle = "rgba(0,0,0,0.4)";
    ctx.beginPath();
    ctx.ellipse(sx + this.w / 2, sy + this.h + 2, 14, 4, 0, 0, Math.PI * 2);
    ctx.fill();
    ctx.restore();

    // cuerpo (silueta oscura con un sutil contorno azulado)
    ctx.save();
    ctx.fillStyle = "#05060a";
    ctx.strokeStyle = "rgba(90,120,170,0.25)";
    ctx.lineWidth = 1;

    const cx = sx + this.w / 2;
    const swing = this.onGround ? Math.sin(this.animTime * 6) * 4 : 0;
    const armSwing = this.onGround ? Math.sin(this.animTime * 6) * 6 : -3;
    const crouch = this.onGround ? 0 : 2;

    // piernas
    ctx.beginPath();
    ctx.rect(cx - 6, sy + 30 - crouch, 4, 16 + swing);
    ctx.rect(cx + 2, sy + 30 - crouch, 4, 16 - swing);
    ctx.fill();

    // torso (rect en vez de roundRect para compatibilidad con navegadores viejos)
    ctx.beginPath();
    ctx.rect(cx - 7, sy + 16 - crouch, 14, 18);
    ctx.fill();

    // cabeza
    ctx.beginPath();
    ctx.arc(cx, sy + 9 - crouch, 7, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();

    // brazos
    ctx.beginPath();
    ctx.rect(cx - 10, sy + 18 - crouch, 3, 12 + armSwing);
    ctx.rect(cx + 7,  sy + 18 - crouch, 3, 12 - armSwing);
    ctx.fill();

    // pequeño brillo en los ojos (detalle inquietante tipo Inside)
    ctx.fillStyle = "rgba(200,220,255,0.8)";
    const eyeX = cx + this.facing * 2.5;
    ctx.fillRect(eyeX - 1, sy + 8 - crouch, 2, 2);

    ctx.restore();
  }
}
