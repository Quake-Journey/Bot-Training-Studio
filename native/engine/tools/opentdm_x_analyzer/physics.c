/* Offline inverse movement search. Uses MAPGEN's BSP binding and the engine's
 * unchanged PmoveOld. A fitted command is a possible witness, not the original
 * user input; unresolved knockback/mover/rules cases remain unresolved. */
#include "common/mapgen_pmove.h"
#include "common/mapgen_trace.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define OTXF_EXPORT __declspec(dllexport)
#else
#define OTXF_EXPORT __attribute__((visibility("default")))
#endif

static mapgen_bsp_t *world;
static mapgen_movers_t movers;
static mapgen_trace_context_t scene_trace;

OTXF_EXPORT void OTXF_Close(void)
{
    MapGenPmove_Release();
    MapGenTrace_Release(&scene_trace);
    MapGenBsp_Free(world);
    world = NULL;
}

OTXF_EXPORT int OTXF_Open(const void *bytes, size_t length)
{
    if (world || !bytes || !length) return 0;
    if (MapGenBsp_Load(bytes, length, &world) != MAPGEN_BSP_OK) return 0;
    if (!MapGenMovers_Read(world, &movers) || movers.overflowed ||
        MapGenPmove_BindWorld(world, &movers) != MAPGEN_PMOVE_OK ||
        !MapGenTrace_Bind(&scene_trace,world)) {
        OTXF_Close();
        return 0;
    }
    return 1;
}

/* Offline scene queries reuse MAPGEN collision. Recorded translated inline
 * models are explicit inputs; unknown/rotated models must be marked unresolved
 * by the caller, not silently treated as transparent. No player bodies here. */
OTXF_EXPORT int OTXF_ModelCount(void)
{
    return world ? (int)MapGenBsp_NumModels(world) : 0;
}

/* Studio-only detailed static trace: fraction/startsolid/endpos/normal.
 * Dynamic entity presence and projectile/player collisions remain explicit
 * qualification limits. This does not modify a server game module. */
OTXF_EXPORT int BTS_Trace(const float *start, const float *end, int mask, float *out)
{
    const float zero[3] = {0, 0, 0};
    if (!world || !start || !end || !out || mask <= 0) return 0;
    for (int i = 0; i < 3; i++)
        if (!isfinite(start[i]) || !isfinite(end[i])) return 0;
    mapgen_trace_result_t hit;
    MapGenTrace_Box(&scene_trace, start, end, zero, zero, mask, &hit);
    out[0] = hit.fraction; out[1] = hit.startsolid || hit.allsolid;
    memcpy(out + 2, hit.endpos, sizeof(float) * 3);
    memcpy(out + 5, hit.plane_normal, sizeof(float) * 3);
    return 1;
}
/* Navigation support is separate from inverse-demo movement fitting. Observed
 * air samples first need a real standing support; links then need an actual
 * ordinary-input traversal from rest. Static movers stay at their first stop. */
OTXF_EXPORT int OTXF_NavSettle(const float *points,int count,float *out)
{
    if(!world || !points || !out || count<1 || count>32768)return 0;
    for(int i=0;i<count*3;i++)if(!isfinite(points[i]) || fabsf(points[i])>=32768)return 0;
    for(int i=0;i<count;i++) {
        mapgen_pmove_player_t p;float start[3],end[3];
        memcpy(start,points+i*3,sizeof(start));start[2]+=1;
        out[i*4+3]=0;
        if(!MapGenPmove_SweepBody(start,start,end))continue;
        MapGenPmove_Spawn(&p,start);
        if(!MapGenPmove_DropToFloor(&p) || !p.on_ground || p.water_level ||
           start[2]-p.origin[2]>160 || !MapGenPmove_SweepBody(p.origin,p.origin,end))continue;
        memcpy(out+i*4,p.origin,3*sizeof(float));out[i*4+3]=1;
    }
    return 1;
}
OTXF_EXPORT int OTXF_NavLinks(const float *pairs,int count,float *out)
{
    if(!world || !pairs || !out || count<1 || count>1024)return 0;
    for(int i=0;i<count*6;i++)if(!isfinite(pairs[i]) || fabsf(pairs[i])>=32768)return 0;
    for(int i=0;i<count;i++) {
        const float *start=pairs+i*6,*goal=start+3;
        float dz=goal[2]-start[2];out[i*2]=out[i*2+1]=0;
        if(fabsf(dz)>64 || hypotf(goal[0]-start[0],goal[1]-start[1])>100)continue;
        for(int jump=0;jump<=1 && !out[i*2];jump++) {
            if(jump && (dz<=18 || dz>64))continue;
            mapgen_pmove_player_t p;MapGenPmove_Spawn(&p,start);
            for(int frame=0;frame<18;frame++) {
                float dx=goal[0]-p.origin[0],dy=goal[1]-p.origin[1],distance=hypotf(dx,dy);
                mapgen_pmove_command_t cmd={0};cmd.msec=100;
                cmd.yaw=atan2f(dy,dx)*57.2957795f;
                cmd.forward=(int)fminf(400,distance*10);cmd.up=jump && !frame ? 400 : 0;
                float vz=p.velocity[2];
                if(!MapGenPmove_Step(&p,&cmd) || p.water_level || (p.on_ground && vz< -500))break;
                distance=hypotf(goal[0]-p.origin[0],goal[1]-p.origin[1]);
                if(p.on_ground && distance<=8 && fabsf(goal[2]-p.origin[2])<=4) {
                    out[i*2]=(frame+1)*.1f;out[i*2+1]=(float)jump;break;
                }
                if(p.origin[2]<fminf(start[2],goal[2])-48)break;
            }
        }
    }
    return 1;
}
OTXF_EXPORT int OTXF_ModelEnvelope(int model,float *bounds)
{
    if(!world || !bounds || model<=0 || model>=(int)MapGenBsp_NumModels(world))return 0;
    for(unsigned i=0;i<movers.num_movers;i++) {
        const mapgen_mover_t *m=&movers.movers[i];
        /* Only the verified ordinary translating lift model is used here.
         * A missing train/rotator/custom entity remains unknown. */
        if((int)m->model!=model || strcmp(m->classname,"func_plat") || m->num_stops!=2)continue;
        for(int k=0;k<3;k++) {
            bounds[k]=m->mins[k]+fminf(m->stop[0][k],m->stop[1][k])-1;
            bounds[k+3]=m->maxs[k]+fmaxf(m->stop[0][k],m->stop[1][k])+1;
        }
        return 1;
    }
    return 0;
}
OTXF_EXPORT int OTXF_SceneRays(const float *rays,int count,const int *models,
                               const float *origins,int model_count,int mask,float *result)
{
    const float zero[3]={0,0,0};
    if(!world || !rays || !result || count<1 || count>256 || model_count<0 ||
       model_count>1024 || (model_count && (!models || !origins)) || mask<=0)return 0;
    for(int i=0;i<count*6;i++)if(!isfinite(rays[i]))return 0;
    for(int i=0;i<model_count;i++) {
        if(models[i]<=0 || models[i]>=(int)MapGenBsp_NumModels(world))return 0;
        for(int k=0;k<3;k++)if(!isfinite(origins[i*3+k]))return 0;
    }
    for(int i=0;i<count;i++) {
        mapgen_trace_result_t hit;
        MapGenTrace_Box(&scene_trace,rays+i*6,rays+i*6+3,zero,zero,mask,&hit);
        for(int j=0;j<model_count;j++)
            MapGenTrace_BoxModel(&scene_trace,models[j],origins+j*3,rays+i*6,rays+i*6+3,zero,zero,mask,&hit);
        result[i*3]=hit.fraction;result[i*3+1]=hit.startsolid;result[i*3+2]=hit.allsolid;
    }
    return 1;
}

static float angle_delta(float a, float b)
{
    float d = fmodf(b - a, 360.f);
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    return d;
}

static int jump_input(int pattern, int elapsed)
{
    switch (pattern) {
    case 1: return 400;                 /* held jump */
    case 2: return elapsed < 20 ? 400 : 0;
    case 3: return elapsed >= 50 ? 400 : 0;
    case 4: return elapsed < 20 || elapsed >= 60 ? 400 : 0;
    case 5: return -400;                /* crouch */
    case 6: return elapsed < 30 || elapsed >= 60 ? 400 : 0; /* PO +2j: three 10ms waits per edge */
    default: return 0;
    }
}

static void unpack(mapgen_pmove_player_t *p, const float *a, const int *meta)
{
    memset(p, 0, sizeof(*p));
    memcpy(p->origin, a, 3 * sizeof(float));
    memcpy(p->velocity, a + 3, 3 * sizeof(float));
    memcpy(p->view_angles, a + 6, 3 * sizeof(float));
    p->pm_flags = meta[0]; p->pm_time = meta[1]; p->gravity = meta[2];
    p->pm_type = meta[3]; p->on_ground = (meta[0] & 4) != 0;
}

/* a/b = position[3], velocity[3], view[3]. meta = old flags/time/gravity/type,
 * observed interval milliseconds. Result: forward, side, jump-pattern,
 * substep-ms, endpoint errors (position, velocity), ground contacts and jump
 * events, simulated endpoint[3], objective. These arrays are a local FFI only,
 * never a persistent native-struct dump. */
OTXF_EXPORT int OTXF_Fit(const float *a, const float *b, const int *meta, float *result)
{
    static const int moves[] = {0, 400, -400};
    static const int steps[] = {7, 8, 10, 16, 100};
    float best = 1e30f;
    int f, s, pattern, step;
    if (!world || !a || !b || !meta || !result || meta[4] != 100 || meta[3] != 0)
        return 0;
    for (int k = 0; k < 9; k++) if (!isfinite(a[k]) || !isfinite(b[k])) return 0;
    if (meta[2] <= 0 || meta[2] > 4000 || meta[0] & 32) return 0;
    for (f = 0; f < 3; f++) for (s = 0; s < 3; s++)
    for (pattern = 0; pattern < 7; pattern++) for (step = 0; step < (int)(sizeof(steps)/sizeof(steps[0])); step++) {
        mapgen_pmove_player_t player;
        float pe = 0, ve = 0, objective;
        int elapsed = 0, contacts = 0, jumps = 0;
        unpack(&player, a, meta);
        while (elapsed < 100) {
            int dt = steps[step] < 100 - elapsed ? steps[step] : 100 - elapsed;
            mapgen_pmove_command_t cmd = {0};
            int ground_before = player.on_ground, flags_before=player.pm_flags;
            float blend = (elapsed + dt) / 100.f;
            cmd.msec = dt;
            cmd.forward = moves[f]; cmd.side = moves[s];
            cmd.up = jump_input(pattern, elapsed);
            cmd.yaw = a[7] + angle_delta(a[7], b[7]) * blend;
            cmd.pitch = a[6] + angle_delta(a[6], b[6]) * blend;
            if (!MapGenPmove_Step(&player, &cmd)) return 0;
            if (!ground_before && player.on_ground) contacts++;
            /* Same jump impulse signal as OpenTDM ClientThink. Ground may be
             * discovered INSIDE this substep; the prior on_ground bit misses it. */
            if ((~flags_before & player.pm_flags & 2) && player.water_level==0) jumps++;
            elapsed += dt;
        }
        for (int k = 0; k < 3; k++) {
            float d = player.origin[k] - b[k]; pe += d * d;
            d = player.velocity[k] - b[k + 3]; ve += d * d;
        }
        pe = sqrtf(pe); ve = sqrtf(ve);
        /* Recorded endpoint and velocity dominate; tiny deterministic
         * complexity penalty breaks ties in favour of ordinary movement. */
        objective = pe + .04f * ve + .0001f * pattern;
        if (objective < best) {
            best = objective;
            result[0] = moves[f]; result[1] = moves[s]; result[2] = pattern;
            result[3] = steps[step]; result[4] = pe; result[5] = ve;
            result[6] = contacts; result[7] = jumps;
            memcpy(result + 8, player.origin, 3 * sizeof(float));
            result[11] = objective;
        }
    }
    return 1;
}

/* Replay a whole sequence WITHOUT resetting to the recorded state each tick.
 * controls[frames][4] = forward, side, jump pattern, substep milliseconds.
 * target[frames][9] = recorded endpoint position, velocity and view angles.
 * output[frames][8] = position/velocity error, jump/land counts, simulated
 * position and flags. A result remains a conditional static-world witness. */
OTXF_EXPORT int OTXF_Replay(const float *initial,const int *meta,
                            const int *controls,const float *target,int frames,float *output)
{
    mapgen_pmove_player_t player;
    if(!world || !initial || !meta || !controls || !target || !output || frames<1 || frames>40 ||
       meta[2]<=0 || meta[2]>4000 || meta[3]!=0 || meta[0]&32)return 0;
    for(int k=0;k<9;k++)if(!isfinite(initial[k]))return 0;
    for(int i=0;i<frames;i++) {
        const int *ctrl=controls+i*4;
        if(ctrl[0]<-400 || ctrl[0]>400 || ctrl[1]<-400 || ctrl[1]>400 || ctrl[2]<0 || ctrl[2]>6 ||
           (ctrl[3]!=7 && ctrl[3]!=8 && ctrl[3]!=10 && ctrl[3]!=16 && ctrl[3]!=100))return 0;
        for(int k=0;k<9;k++)if(!isfinite(target[i*9+k]))return 0;
    }
    unpack(&player,initial,meta);
    for(int i=0;i<frames;i++) {
        const int *ctrl=controls+i*4;const float *ref=target+i*9;
        float start_pitch=player.view_angles[0],start_yaw=player.view_angles[1];
        float *result=output+i*8,pe=0,ve=0;int elapsed=0,jumps=0,lands=0;
        while(elapsed<100) {
            int dt=ctrl[3]<100-elapsed ? ctrl[3] : 100-elapsed;
            int grounded=player.on_ground,flags_before=player.pm_flags;float blend=(elapsed+dt)/100.f;
            mapgen_pmove_command_t cmd={0};
            cmd.msec=dt;cmd.forward=ctrl[0];cmd.side=ctrl[1];cmd.up=jump_input(ctrl[2],elapsed);
            cmd.pitch=start_pitch+angle_delta(start_pitch,ref[6])*blend;
            cmd.yaw=start_yaw+angle_delta(start_yaw,ref[7])*blend;
            if(!MapGenPmove_Step(&player,&cmd))return 0;
            if(!grounded && player.on_ground)lands++;
            if((~flags_before & player.pm_flags & 2) && player.water_level==0)jumps++;
            elapsed+=dt;
        }
        for(int k=0;k<3;k++) {
            float d=player.origin[k]-ref[k];pe+=d*d;
            d=player.velocity[k]-ref[k+3];ve+=d*d;
        }
        result[0]=sqrtf(pe);result[1]=sqrtf(ve);result[2]=jumps;result[3]=lands;
        memcpy(result+4,player.origin,3*sizeof(float));result[7]=player.pm_flags;
    }
    return 1;
}

/* Fit from the simulated previous endpoint. Unlike independent 100ms fits,
 * this cannot hide drift by snapping the body back to the recording. */
OTXF_EXPORT int BTS_FitSequence(const float *initial, const int *meta,
                               const float *target, int frames, int *controls,
                               float *output)
{
    mapgen_pmove_player_t player;
    if (!world || !initial || !meta || !target || !controls || !output ||
        frames < 1 || frames > 40 || meta[2] <= 0 || meta[2] > 4000 || meta[3] != 0 || meta[0] & 32)
        return 0;
    for (int i = 0; i < 9; i++) if (!isfinite(initial[i])) return 0;
    for (int i = 0; i < frames * 9; i++) if (!isfinite(target[i])) return 0;
    unpack(&player, initial, meta);
    for (int i = 0; i < frames; i++) {
        float from[9], fit[12];
        memcpy(from, player.origin, 3*sizeof(float));
        memcpy(from+3, player.velocity, 3*sizeof(float));
        memcpy(from+6, player.view_angles, 3*sizeof(float));
        int current[5] = {player.pm_flags, player.pm_time, player.gravity, player.pm_type, 100};
        const float *ref = target + i*9;
        if (!OTXF_Fit(from, ref, current, fit)) return 0;
        for (int k = 0; k < 4; k++) controls[i*4+k] = (int)fit[k];
        int elapsed=0, jumps=0, lands=0;
        while (elapsed<100) {
            int dt=controls[i*4+3]; if(dt>100-elapsed)dt=100-elapsed;
            int flags=player.pm_flags, grounded=player.on_ground;
            float blend=(elapsed+dt)/100.f;
            mapgen_pmove_command_t cmd={0};cmd.msec=dt;
            cmd.forward=controls[i*4];cmd.side=controls[i*4+1];
            cmd.up=jump_input(controls[i*4+2],elapsed);
            cmd.pitch=from[6]+angle_delta(from[6],ref[6])*blend;
            cmd.yaw=from[7]+angle_delta(from[7],ref[7])*blend;
            if(!MapGenPmove_Step(&player,&cmd))return 0;
            if(!grounded && player.on_ground)lands++;
            if((~flags & player.pm_flags & 2) && player.water_level==0)jumps++;
            elapsed+=dt;
        }
        float pe=0,ve=0;
        for(int k=0;k<3;k++) {
            float d=player.origin[k]-ref[k];pe+=d*d;
            d=player.velocity[k]-ref[k+3];ve+=d*d;
        }
        float *result=output+i*8;
        result[0]=sqrtf(pe);result[1]=sqrtf(ve);result[2]=jumps;result[3]=lands;
        memcpy(result+4,player.origin,3*sizeof(float));result[7]=player.pm_flags;
    }
    return 1;
}
