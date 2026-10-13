/* Compatibility repair for the hash-pinned v3 server, not an ABI conversion.
 * CFuncTank::Fire stores a CSprite* at +0x22c (RVA 0xda95e). The sprite's
 * timed Think (0xc4fb0) removes it, but CFuncTank::Think (0xd97e0) continues
 * dereferencing that pointer for bullet mode 3. Native c2a2d combat reproduced
 * the freed allocation being reused by a client buffer. Retire the reference
 * when its actual sprite is destroyed, before the address can be recycled.
 * Constructors establish the four tank vtables and the sprite vtable below.
 * No executable bytes or translated instructions are changed.
 */
static unsigned retired_tank_sprites;
static void v3_retire_sprite(uint32_t sprite)
{
    if(!sprite||rt_r32(G_MEM,sprite)!=0x10200598u)return;
    for(unsigned i=0;i<edict_capacity;i++){
        uint32_t tank=private_data[i];
        if(!tank||edict_base[i].free||!edict_base[i].pvPrivateData)continue;
        uint32_t table=rt_r32(G_MEM,tank);
        if(table!=0x102028acu&&table!=0x1020268cu&&table!=0x1020279cu&&table!=0x10202474u)continue;
        if(rt_r32(G_MEM,tank+0x22c)!=sprite)continue;
        rt_w32(G_MEM,tank+0x22c,0);
        if(!retired_tank_sprites++)fprintf(stderr,"BHL v3 retired an expired mounted-weapon sprite reference\n");
    }
}
