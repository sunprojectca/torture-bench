/*
 * raytracer.c
 * Pure-CPU path tracer. No GPU calls. No SIMD (uses scalar math only).
 * Renders a fixed scene repeatedly — measures rays/sec.
 *
 * If a result is suspiciously fast, suspect GPU offload via driver hooks.
 */

#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── math types ─────────────────────────────────────────────────────────── */
typedef struct { double x, y, z; } vec3;

static inline vec3 v3add(vec3 a, vec3 b) { return (vec3){a.x+b.x,a.y+b.y,a.z+b.z}; }
static inline vec3 v3sub(vec3 a, vec3 b) { return (vec3){a.x-b.x,a.y-b.y,a.z-b.z}; }
static inline vec3 v3mul(vec3 a, double t){ return (vec3){a.x*t,a.y*t,a.z*t}; }
static inline vec3 v3muls(vec3 a, vec3 b) { return (vec3){a.x*b.x,a.y*b.y,a.z*b.z}; }
static inline double v3dot(vec3 a, vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static inline vec3 v3norm(vec3 a) {
    double l = sqrt(v3dot(a,a));
    return l > 0 ? v3mul(a, 1.0/l) : a;
}
static inline vec3 v3cross(vec3 a, vec3 b) {
    return (vec3){ a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x };
}
static inline vec3 v3reflect(vec3 v, vec3 n) {
    return v3sub(v, v3mul(n, 2.0*v3dot(v,n)));
}

/* ── ray ─────────────────────────────────────────────────────────────────── */
typedef struct { vec3 origin, dir; } ray;
static inline vec3 ray_at(ray r, double t) { return v3add(r.origin, v3mul(r.dir, t)); }

/* ── sphere ──────────────────────────────────────────────────────────────── */
typedef struct {
    vec3   center;
    double radius;
    vec3   albedo;
    int    is_light;   /* 1 = emissive */
} sphere;

/* ── scene ───────────────────────────────────────────────────────────────── */
#define MAX_SPHERES 12
static sphere scene[MAX_SPHERES];
static int    num_spheres = 0;

static void scene_init(void) {
    num_spheres = 0;
    /* Ground */
    scene[num_spheres++] = (sphere){{0,-1000,0},1000,{0.5,0.5,0.5},0};
    /* Large glass-like sphere */
    scene[num_spheres++] = (sphere){{0,1,0},1.0,{0.9,0.9,0.9},0};
    /* Diffuse sphere */
    scene[num_spheres++] = (sphere){{-4,1,0},1.0,{0.4,0.2,0.1},0};
    /* Metal sphere */
    scene[num_spheres++] = (sphere){{4,1,0},1.0,{0.7,0.6,0.5},0};
    /* Small accent spheres */
    scene[num_spheres++] = (sphere){{2,0.5,2},0.5,{0.8,0.3,0.3},0};
    scene[num_spheres++] = (sphere){{-2,0.5,2},0.5,{0.3,0.8,0.3},0};
    scene[num_spheres++] = (sphere){{0,0.5,3},0.5,{0.3,0.3,0.8},0};
    /* Light source */
    scene[num_spheres++] = (sphere){{0,8,-2},2.0,{5,5,5},1};
}

static int sphere_hit(const sphere *s, ray r, double tmin, double tmax, double *t_out) {
    vec3   oc = v3sub(r.origin, s->center);
    double a  = v3dot(r.dir, r.dir);
    double hb = v3dot(oc, r.dir);
    double c  = v3dot(oc, oc) - s->radius * s->radius;
    double disc = hb*hb - a*c;
    if (disc < 0) return 0;
    double sqd = sqrt(disc);
    double root = (-hb - sqd) / a;
    if (root < tmin || root > tmax) {
        root = (-hb + sqd) / a;
        if (root < tmin || root > tmax) return 0;
    }
    *t_out = root;
    return 1;
}

/* ── random unit vector via xorshift ────────────────────────────────────── */
static vec3 rand_unit(uint64_t *rng) {
    for (;;) {
        double x = (double)(xorshift64(rng) & 0xFFFF) / 65535.0 * 2 - 1;
        double y = (double)(xorshift64(rng) & 0xFFFF) / 65535.0 * 2 - 1;
        double z = (double)(xorshift64(rng) & 0xFFFF) / 65535.0 * 2 - 1;
        vec3 v = {x, y, z};
        if (v3dot(v, v) < 1.0) return v3norm(v);
    }
}

/* ── path trace one ray, max_depth bounces ──────────────────────────────── */
static vec3 trace(ray r, int depth, uint64_t *rng) {
    if (depth <= 0) return (vec3){0,0,0};

    double t_closest = 1e18;
    int    hit_idx   = -1;

    for (int i = 0; i < num_spheres; i++) {
        double t;
        if (sphere_hit(&scene[i], r, 1e-4, t_closest, &t)) {
            t_closest = t;
            hit_idx   = i;
        }
    }

    if (hit_idx < 0) {
        /* Sky gradient */
        vec3   d = v3norm(r.dir);
        double t = 0.5 * (d.y + 1.0);
        vec3 sky_top = {0.5, 0.7, 1.0};
        vec3 sky_bot = {1.0, 1.0, 1.0};
        return v3add(v3mul(sky_bot, 1.0-t), v3mul(sky_top, t));
    }

    const sphere *s  = &scene[hit_idx];
    vec3  hit_p  = ray_at(r, t_closest);
    vec3  normal = v3norm(v3sub(hit_p, s->center));

    if (s->is_light) return s->albedo;

    /* Lambertian diffuse */
    vec3 scatter_dir = v3add(normal, rand_unit(rng));
    ray  scattered   = { hit_p, v3norm(scatter_dir) };
    vec3 recurse     = trace(scattered, depth - 1, rng);

    return v3muls(s->albedo, recurse);
}

/* ── render one tile (WIDTH x HEIGHT pixels, SPP samples per pixel) ──────── */
#define RT_WIDTH  64
#define RT_HEIGHT 48
#define RT_SPP    4
#define RT_DEPTH  6

static double render_tile(uint64_t seed) {
    uint64_t rng = seed;

    /* Camera setup */
    vec3   origin    = {13, 2, 3};
    vec3   lookat    = {0, 0, 0};
    vec3   vup       = {0, 1, 0};
    double vfov      = 20.0 * (3.14159265358979 / 180.0);
    double aspect    = (double)RT_WIDTH / RT_HEIGHT;
    double h         = tan(vfov / 2.0);
    double vp_h      = 2.0 * h;
    double vp_w      = aspect * vp_h;
    vec3   w         = v3norm(v3sub(origin, lookat));
    vec3   u         = v3norm(v3cross(vup, w));
    vec3   v_axis    = v3cross(w, u);
    vec3   horiz     = v3mul(u, vp_w);
    vec3   vert      = v3mul(v_axis, vp_h);
    vec3   ll_corner = v3sub(v3sub(v3sub(origin, v3mul(horiz, 0.5)),
                                   v3mul(vert, 0.5)), w);

    double acc = 0.0; /* accumulate brightness to prevent dead-code elim */

    for (int j = 0; j < RT_HEIGHT; j++) {
        for (int i = 0; i < RT_WIDTH; i++) {
            vec3 pixel = {0,0,0};
            for (int s = 0; s < RT_SPP; s++) {
                double ru = ((double)(xorshift64(&rng) & 0xFFFF)) / 65535.0;
                double rv = ((double)(xorshift64(&rng) & 0xFFFF)) / 65535.0;
                double uu = ((double)i + ru) / (RT_WIDTH  - 1);
                double vv = ((double)j + rv) / (RT_HEIGHT - 1);
                vec3 dir = v3sub(
                    v3add(v3add(ll_corner, v3mul(horiz, uu)), v3mul(vert, vv)),
                    origin);
                ray r = { origin, v3norm(dir) };
                vec3 c = trace(r, RT_DEPTH, &rng);
                pixel = v3add(pixel, c);
            }
            acc += pixel.x + pixel.y + pixel.z;
        }
    }
    return acc;
}

/* ── worker ──────────────────────────────────────────────────────────────── */
static void *raytrace_worker(void *arg) {
    parallel_arg_t *a = (parallel_arg_t *)arg;
    platform_pin_thread(a->core_id);

    uint64_t frames = 0;
    double acc = 0.0;
    uint64_t seed = a->seed;
    double deadline = bench_now_sec() + a->duration_sec;

    while (bench_now_sec() < deadline) {
        acc += render_tile(xorshift64(&seed));
        frames++;
    }

    a->ops = (double)frames * RT_WIDTH * RT_HEIGHT * RT_SPP;
    a->hash_out = mix64(a->seed, (uint64_t)(acc * 1e6));
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_raytracer(uint64_t chain_seed,
                                 int thread_count, int duration_sec) {
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "raytracer";
    r.chain_in    = chain_seed;

    scene_init();

    int nthreads = resolve_threads(thread_count);
    double total_ops = 0.0;
    uint64_t combined_hash = chain_seed;
    parallel_run(nthreads, chain_seed, duration_sec, raytrace_worker, &total_ops, &combined_hash);

    double elapsed = (double)duration_sec;
    double rays_sec = total_ops / elapsed;

    r.wall_time_sec  = elapsed;
    r.ops_per_sec    = rays_sec;
    r.score          = rays_sec / 1000.0;
    r.chain_out      = mix64(chain_seed, combined_hash);

    if (rays_sec / nthreads > 2000000.0) {
        r.coprocessor_suspected = 1;
        snprintf(r.flags, sizeof(r.flags),
            "WARN: %.0f rays/sec/core %dT", rays_sec / nthreads, nthreads);
    } else {
        snprintf(r.flags, sizeof(r.flags),
            "PURE_C_SCALAR %dT %.0fk rays/s", nthreads, rays_sec / 1000.0);
    }
    return r;
}
