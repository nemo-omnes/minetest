/*
Minetest
Copyright (C) 2010-2015 kwolekr, Ryan Kwolek <kwolekr@minetest.net>
Copyright (C) 2010-2015 paramat, Matt Gregory

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/


#include "mapgen.h"
#include "voxel.h"
#include "noise.h"
#include "mapblock.h"
#include "mapnode.h"
#include "map.h"
#include "content_sao.h"
#include "nodedef.h"
#include "voxelalgorithms.h"
//#include "profiler.h" // For TimeTaker
#include "settings.h" // For g_settings
#include "emerge.h"
#include "dungeongen.h"
#include "cavegen.h"
#include "treegen.h"
#include "mg_biome.h"
#include "mg_ore.h"
#include "mg_decoration.h"
#include "mapgen_fractal.h"


FlagDesc flagdesc_mapgen_fractal[] = {
	{NULL,    0}
};

///////////////////////////////////////////////////////////////////////////////////////


MapgenFractal::MapgenFractal(int mapgenid, MapgenParams *params, EmergeManager *emerge)
	: MapgenBasic(mapgenid, params, emerge)
{
	this->m_emerge = emerge;
	this->bmgr     = emerge->biomemgr;

	//// amount of elements to skip for the next index
	//// for noise/height/biome maps (not vmanip)
	this->ystride = csize.X;

	this->heightmap = new s16[csize.X * csize.Z];

	MapgenFractalParams *sp = (MapgenFractalParams *)params->sparams;

	this->spflags    = sp->spflags;
	this->cave_width = sp->cave_width;
	this->fractal    = sp->fractal;
	this->iterations = sp->iterations;
	this->scale      = sp->scale;
	this->offset     = sp->offset;
	this->slice_w    = sp->slice_w;
	this->julia_x    = sp->julia_x;
	this->julia_y    = sp->julia_y;
	this->julia_z    = sp->julia_z;
	this->julia_w    = sp->julia_w;

	//// 2D terrain noise
	noise_seabed       = new Noise(&sp->np_seabed, seed, csize.X, csize.Z);
	noise_filler_depth = new Noise(&sp->np_filler_depth, seed, csize.X, csize.Z);

	MapgenBasic::np_cave1 = sp->np_cave1;
	MapgenBasic::np_cave2 = sp->np_cave2;

	//// Initialize biome generator
	biomegen = emerge->biomemgr->createBiomeGen(
		BIOMEGEN_ORIGINAL, params->bparams, csize);
	biomemap = biomegen->biomemap;

	this->formula = fractal / 2 + fractal % 2;
	this->julia   = fractal % 2 == 0;

	//// Resolve nodes to be used
	c_stone                = ndef->getId("mapgen_stone");
	c_water_source         = ndef->getId("mapgen_water_source");
	c_lava_source          = ndef->getId("mapgen_lava_source");
	c_desert_stone         = ndef->getId("mapgen_desert_stone");
	c_ice                  = ndef->getId("mapgen_ice");
	c_sandstone            = ndef->getId("mapgen_sandstone");

	c_cobble               = ndef->getId("mapgen_cobble");
	c_stair_cobble         = ndef->getId("mapgen_stair_cobble");
	c_mossycobble          = ndef->getId("mapgen_mossycobble");
	c_sandstonebrick       = ndef->getId("mapgen_sandstonebrick");
	c_stair_sandstonebrick = ndef->getId("mapgen_stair_sandstonebrick");

	if (c_ice == CONTENT_IGNORE)
		c_ice = CONTENT_AIR;
	if (c_mossycobble == CONTENT_IGNORE)
		c_mossycobble = c_cobble;
	if (c_stair_cobble == CONTENT_IGNORE)
		c_stair_cobble = c_cobble;
	if (c_sandstonebrick == CONTENT_IGNORE)
		c_sandstonebrick = c_sandstone;
	if (c_stair_sandstonebrick == CONTENT_IGNORE)
		c_stair_sandstonebrick = c_sandstone;
}


MapgenFractal::~MapgenFractal()
{
	delete noise_seabed;
	delete noise_filler_depth;

	delete biomegen;

	delete[] heightmap;
}


MapgenFractalParams::MapgenFractalParams()
{
	spflags    = 0;
	cave_width = 0.3;
	fractal    = 1;
	iterations = 11;
	scale      = v3f(4096.0, 1024.0, 4096.0);
	offset     = v3f(1.79, 0.0, 0.0);
	slice_w    = 0.0;
	julia_x    = 0.33;
	julia_y    = 0.33;
	julia_z    = 0.33;
	julia_w    = 0.33;

	np_seabed       = NoiseParams(-14, 9,   v3f(600, 600, 600), 41900, 5, 0.6, 2.0);
	np_filler_depth = NoiseParams(0,   1.2, v3f(150, 150, 150), 261,   3, 0.7, 2.0);
	np_cave1        = NoiseParams(0,   12,  v3f(96,  96,  96),  52534, 4, 0.5, 2.0);
	np_cave2        = NoiseParams(0,   12,  v3f(96,  96,  96),  10325, 4, 0.5, 2.0);
}


void MapgenFractalParams::readParams(const Settings *settings)
{
	settings->getFlagStrNoEx("mgfractal_spflags",  spflags, flagdesc_mapgen_fractal);
	settings->getFloatNoEx("mgfractal_cave_width", cave_width);
	settings->getU16NoEx("mgfractal_fractal",      fractal);
	settings->getU16NoEx("mgfractal_iterations",   iterations);
	settings->getV3FNoEx("mgfractal_scale",        scale);
	settings->getV3FNoEx("mgfractal_offset",       offset);
	settings->getFloatNoEx("mgfractal_slice_w",    slice_w);
	settings->getFloatNoEx("mgfractal_julia_x",    julia_x);
	settings->getFloatNoEx("mgfractal_julia_y",    julia_y);
	settings->getFloatNoEx("mgfractal_julia_z",    julia_z);
	settings->getFloatNoEx("mgfractal_julia_w",    julia_w);

	settings->getNoiseParams("mgfractal_np_seabed",       np_seabed);
	settings->getNoiseParams("mgfractal_np_filler_depth", np_filler_depth);
	settings->getNoiseParams("mgfractal_np_cave1",        np_cave1);
	settings->getNoiseParams("mgfractal_np_cave2",        np_cave2);
}


void MapgenFractalParams::writeParams(Settings *settings) const
{
	settings->setFlagStr("mgfractal_spflags",  spflags, flagdesc_mapgen_fractal, U32_MAX);
	settings->setFloat("mgfractal_cave_width", cave_width);
	settings->setU16("mgfractal_fractal",      fractal);
	settings->setU16("mgfractal_iterations",   iterations);
	settings->setV3F("mgfractal_scale",        scale);
	settings->setV3F("mgfractal_offset",       offset);
	settings->setFloat("mgfractal_slice_w",    slice_w);
	settings->setFloat("mgfractal_julia_x",    julia_x);
	settings->setFloat("mgfractal_julia_y",    julia_y);
	settings->setFloat("mgfractal_julia_z",    julia_z);
	settings->setFloat("mgfractal_julia_w",    julia_w);

	settings->setNoiseParams("mgfractal_np_seabed",       np_seabed);
	settings->setNoiseParams("mgfractal_np_filler_depth", np_filler_depth);
	settings->setNoiseParams("mgfractal_np_cave1",        np_cave1);
	settings->setNoiseParams("mgfractal_np_cave2",        np_cave2);
}


/////////////////////////////////////////////////////////////////


int MapgenFractal::getSpawnLevelAtPoint(v2s16 p)
{
	bool solid_below = false;  // Dry solid node is present below to spawn on
	u8 air_count = 0;  // Consecutive air nodes above the dry solid node
	s16 seabed_level = NoisePerlin2D(&noise_seabed->np, p.X, p.Y, seed);
	// Seabed can rise above water_level or might be raised to create dry land
	s16 search_start = MYMAX(seabed_level, water_level + 1);
	if (seabed_level > water_level)
		solid_below = true;

	for (s16 y = search_start; y <= search_start + 128; y++) {
		if (getFractalAtPoint(p.X, y, p.Y)) {  // Fractal node
			solid_below = true;
			air_count = 0;
		} else if (solid_below) {  // Air above solid node
			air_count++;
			if (air_count == 2)
				return y - 2;
		}
	}

	return MAX_MAP_GENERATION_LIMIT;  // Unsuitable spawn point
}


void MapgenFractal::makeChunk(BlockMakeData *data)
{
	// Pre-conditions
	assert(data->vmanip);
	assert(data->nodedef);
	assert(data->blockpos_requested.X >= data->blockpos_min.X &&
		data->blockpos_requested.Y >= data->blockpos_min.Y &&
		data->blockpos_requested.Z >= data->blockpos_min.Z);
	assert(data->blockpos_requested.X <= data->blockpos_max.X &&
		data->blockpos_requested.Y <= data->blockpos_max.Y &&
		data->blockpos_requested.Z <= data->blockpos_max.Z);

	this->generating = true;
	this->vm   = data->vmanip;
	this->ndef = data->nodedef;
	//TimeTaker t("makeChunk");

	v3s16 blockpos_min = data->blockpos_min;
	v3s16 blockpos_max = data->blockpos_max;
	node_min = blockpos_min * MAP_BLOCKSIZE;
	node_max = (blockpos_max + v3s16(1, 1, 1)) * MAP_BLOCKSIZE - v3s16(1, 1, 1);
	full_node_min = (blockpos_min - 1) * MAP_BLOCKSIZE;
	full_node_max = (blockpos_max + 2) * MAP_BLOCKSIZE - v3s16(1, 1, 1);

	blockseed = getBlockSeed2(full_node_min, seed);

	// Make some noise
	calculateNoise();

	// Generate base terrain, mountains, and ridges with initial heightmaps
	s16 stone_surface_max_y = generateTerrain();

	// Create heightmap
	updateHeightmap(node_min, node_max);

	// Init biome generator, place biome-specific nodes, and build biomemap
	biomegen->calcBiomeNoise(node_min);
	biomegen->getBiomes(heightmap);
	MgStoneType stone_type = generateBiomes();

	if (flags & MG_CAVES)
		generateCaves(stone_surface_max_y, MGFRACTAL_LARGE_CAVE_DEPTH);

	if ((flags & MG_DUNGEONS) && (stone_surface_max_y >= node_min.Y)) {
		DungeonParams dp;

		dp.np_rarity  = nparams_dungeon_rarity;
		dp.np_density = nparams_dungeon_density;
		dp.np_wetness = nparams_dungeon_wetness;
		dp.c_water    = c_water_source;
		if (stone_type == MGSTONE_STONE) {
			dp.c_cobble = c_cobble;
			dp.c_moss   = c_mossycobble;
			dp.c_stair  = c_stair_cobble;

			dp.diagonal_dirs = false;
			dp.mossratio     = 3.0;
			dp.holesize      = v3s16(1, 2, 1);
			dp.roomsize      = v3s16(0, 0, 0);
			dp.notifytype    = GENNOTIFY_DUNGEON;
		} else if (stone_type == MGSTONE_DESERT_STONE) {
			dp.c_cobble = c_desert_stone;
			dp.c_moss   = c_desert_stone;
			dp.c_stair  = c_desert_stone;

			dp.diagonal_dirs = true;
			dp.mossratio     = 0.0;
			dp.holesize      = v3s16(2, 3, 2);
			dp.roomsize      = v3s16(2, 5, 2);
			dp.notifytype    = GENNOTIFY_TEMPLE;
		} else if (stone_type == MGSTONE_SANDSTONE) {
			dp.c_cobble = c_sandstonebrick;
			dp.c_moss   = c_sandstonebrick;
			dp.c_stair  = c_sandstonebrick;

			dp.diagonal_dirs = false;
			dp.mossratio     = 0.0;
			dp.holesize      = v3s16(2, 2, 2);
			dp.roomsize      = v3s16(2, 0, 2);
			dp.notifytype    = GENNOTIFY_DUNGEON;
		}

		DungeonGen dgen(this, &dp);
		dgen.generate(blockseed, full_node_min, full_node_max);
	}

	// Generate the registered decorations
	if (flags & MG_DECORATIONS)
		m_emerge->decomgr->placeAllDecos(this, blockseed, node_min, node_max);

	// Generate the registered ores
	m_emerge->oremgr->placeAllOres(this, blockseed, node_min, node_max);

	// Sprinkle some dust on top after everything else was generated
	dustTopNodes();

	//printf("makeChunk: %dms\n", t.stop());

	updateLiquid(&data->transforming_liquid, full_node_min, full_node_max);

	if (flags & MG_LIGHT)
		calcLighting(node_min - v3s16(0, 1, 0), node_max + v3s16(0, 1, 0),
			full_node_min, full_node_max);

	//setLighting(node_min - v3s16(1, 0, 1) * MAP_BLOCKSIZE,
	//			node_max + v3s16(1, 0, 1) * MAP_BLOCKSIZE, 0xFF);

	this->generating = false;
}


void MapgenFractal::calculateNoise()
{
	//TimeTaker t("calculateNoise", NULL, PRECISION_MICRO);
	s16 x = node_min.X;
	s16 z = node_min.Z;

	noise_seabed->perlinMap2D(x, z);

	// Cave noises are calculated in generateCaves()
	// only if solid terrain is present in mapchunk

	noise_filler_depth->perlinMap2D(x, z);

	//printf("calculateNoise: %dus\n", t.stop());
}


bool MapgenFractal::getFractalAtPoint(s16 x, s16 y, s16 z)
{
	float cx, cy, cz, cw, ox, oy, oz, ow;

	if (julia) {  // Julia set
		cx = julia_x;
		cy = julia_y;
		cz = julia_z;
		cw = julia_w;
		ox = (float)x / scale.X - offset.X;
		oy = (float)y / scale.Y - offset.Y;
		oz = (float)z / scale.Z - offset.Z;
		ow = slice_w;
	} else {  // Mandelbrot set
		cx = (float)x / scale.X - offset.X;
		cy = (float)y / scale.Y - offset.Y;
		cz = (float)z / scale.Z - offset.Z;
		cw = slice_w;
		ox = 0.0f;
		oy = 0.0f;
		oz = 0.0f;
		ow = 0.0f;
	}

	float nx = 0.0f;
	float ny = 0.0f;
	float nz = 0.0f;
	float nw = 0.0f;

	for (u16 iter = 0; iter < iterations; iter++) {

		if (formula == 1) {  // 4D "Roundy"
			nx = ox * ox - oy * oy - oz * oz - ow * ow + cx;
			ny = 2.0f * (ox * oy + oz * ow) + cy;
			nz = 2.0f * (ox * oz + oy * ow) + cz;
			nw = 2.0f * (ox * ow + oy * oz) + cw;
		} else if (formula == 2) {  // 4D "Squarry"
			nx = ox * ox - oy * oy - oz * oz - ow * ow + cx;
			ny = 2.0f * (ox * oy + oz * ow) + cy;
			nz = 2.0f * (ox * oz + oy * ow) + cz;
			nw = 2.0f * (ox * ow - oy * oz) + cw;
		} else if (formula == 3) {  // 4D "Mandy Cousin"
			nx = ox * ox - oy * oy - oz * oz + ow * ow + cx;
			ny = 2.0f * (ox * oy + oz * ow) + cy;
			nz = 2.0f * (ox * oz + oy * ow) + cz;
			nw = 2.0f * (ox * ow + oy * oz) + cw;
		} else if (formula == 4) {  // 4D "Variation"
			nx = ox * ox - oy * oy - oz * oz - ow * ow + cx;
			ny = 2.0f * (ox * oy + oz * ow) + cy;
			nz = 2.0f * (ox * oz - oy * ow) + cz;
			nw = 2.0f * (ox * ow + oy * oz) + cw;
		} else if (formula == 5) {  // 3D "Mandelbrot/Mandelbar"
			nx = ox * ox - oy * oy - oz * oz + cx;
			ny = 2.0f * ox * oy + cy;
			nz = -2.0f * ox * oz + cz;
		} else if (formula == 6) {  // 3D "Christmas Tree"
			// Altering the formula here is necessary to avoid division by zero
			if (fabs(oz) < 0.000000001f) {
				nx = ox * ox - oy * oy - oz * oz + cx;
				ny = 2.0f * oy * ox + cy;
				nz = 4.0f * oz * ox + cz;
			} else {
				float a = (2.0f * ox) / (sqrt(oy * oy + oz * oz));
				nx = ox * ox - oy * oy - oz * oz + cx;
				ny = a * (oy * oy - oz * oz) + cy;
				nz = a * 2.0f * oy * oz + cz;
			}
		} else if (formula == 7) {  // 3D "Mandelbulb"
			if (fabs(oy) < 0.000000001f) {
				nx = ox * ox - oz * oz + cx;
				ny = cy;
				nz = -2.0f * oz * sqrt(ox * ox) + cz;
			} else {
				float a = 1.0f - (oz * oz) / (ox * ox + oy * oy);
				nx = (ox * ox - oy * oy) * a + cx;
				ny = 2.0f * ox * oy * a + cy;
				nz = -2.0f * oz * sqrt(ox * ox + oy * oy) + cz;
			}
		} else if (formula == 8) {  // 3D "Cosine Mandelbulb"
			if (fabs(oy) < 0.000000001f) {
				nx = 2.0f * ox * oz + cx;
				ny = 4.0f * oy * oz + cy;
				nz = oz * oz - ox * ox - oy * oy + cz;
			} else {
				float a = (2.0f * oz) / sqrt(ox * ox + oy * oy);
				nx = (ox * ox - oy * oy) * a + cx;
				ny = 2.0f * ox * oy * a + cy;
				nz = oz * oz - ox * ox - oy * oy + cz;
			}
		} else if (formula == 9) {  // 4D "Mandelbulb"
			float rxy = sqrt(ox * ox + oy * oy);
			float rxyz = sqrt(ox * ox + oy * oy + oz * oz);
			if (fabs(ow) < 0.000000001f && fabs(oz) < 0.000000001f) {
				nx = (ox * ox - oy * oy) + cx;
				ny = 2.0f * ox * oy + cy;
				nz = -2.0f * rxy * oz + cz;
				nw = 2.0f * rxyz * ow + cw;
			} else {
				float a = 1.0f - (ow * ow) / (rxyz * rxyz);
				float b = a * (1.0f - (oz * oz) / (rxy * rxy));
				nx = (ox * ox - oy * oy) * b + cx;
				ny = 2.0f * ox * oy * b + cy;
				nz = -2.0f * rxy * oz * a + cz;
				nw = 2.0f * rxyz * ow + cw;
			}
		}

		if (nx * nx + ny * ny + nz * nz + nw * nw > 4.0f)
			return false;

		ox = nx;
		oy = ny;
		oz = nz;
		ow = nw;
	}

	return true;
}


s16 MapgenFractal::generateTerrain()
{
	MapNode n_air(CONTENT_AIR);
	MapNode n_stone(c_stone);
	MapNode n_water(c_water_source);

	s16 stone_surface_max_y = -MAX_MAP_GENERATION_LIMIT;
	u32 index2d = 0;

	for (s16 z = node_min.Z; z <= node_max.Z; z++) {
		for (s16 y = node_min.Y - 1; y <= node_max.Y + 1; y++) {
			u32 vi = vm->m_area.index(node_min.X, y, z);
			for (s16 x = node_min.X; x <= node_max.X; x++, vi++, index2d++) {
				if (vm->m_data[vi].getContent() == CONTENT_IGNORE) {
					s16 seabed_height = noise_seabed->result[index2d];

					if (y <= seabed_height || getFractalAtPoint(x, y, z)) {
						vm->m_data[vi] = n_stone;
						if (y > stone_surface_max_y)
							stone_surface_max_y = y;
					} else if (y <= water_level) {
						vm->m_data[vi] = n_water;
					} else {
						vm->m_data[vi] = n_air;
					}
				}
			}
			index2d -= ystride;
		}
		index2d += ystride;
	}

	return stone_surface_max_y;
}
