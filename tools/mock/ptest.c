#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern FILE *mock_out; void mock_flush(void);
int main(int c,char**v){
  mock_out=fopen(v[2],"wb");
  player_init(); player_set_volume(0x8000);
  player_open(v[1],0,0);
  const PlayerStatus*s=player_status(); int last=-1;
  int scenario=atoi(v[3]); int paused_once=0, seeked=0;
  for(int i=0;i<200000;i++){
    usleep(1000);
    if(scenario==1 && !paused_once && s->pos>10){ player_pause(1); usleep(200000/SPEED); player_pause(0); paused_once=1; }
    if(scenario==2 && !seeked && s->pos>5){ player_seek(30,0); seeked=1; }
    if(scenario==3 && !seeked && s->pos>5){ player_open(v[4],0,0); seeked=1; }
    if(s->state!=last){ printf("state=%d pos=%.2f gen=%d\n",s->state,s->pos,s->generation); last=s->state; }
    if(s->state==PL_ENDED||s->state==PL_ERROR) break;
  }
  mock_flush(); fclose(mock_out); player_shutdown(); return 0;}
