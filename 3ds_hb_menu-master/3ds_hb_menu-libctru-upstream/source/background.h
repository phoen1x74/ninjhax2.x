#pragma once
#include <3ds.h>

#define WATERBORDERCOLOR (u8[]){121, 127, 128}
#define WATERCOLOR (u8[]){140, 140, 140}

#define BEERBORDERCOLOR (u8[]){240, 240, 240}
#define BEERCOLOR (u8[]){188, 157, 75}

#define BGCOLOR (u8[]){77, 77, 77}

#define BUBBLE_COUNT 15

typedef struct
{
	s32 x, y;
	u8 fade;
}bubble_t;

void initBackground(void);
void updateBackground(void);
void drawBackground(u8 bgColor[3], u8 waterBorderColor[3], u8 waterColor[3]);
