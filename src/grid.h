#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

namespace grid {
// 320×240 사진 한 장에서 칸마다 가로 16점 × 세로 16점 = 256점을 읽습니다.
constexpr int W=320, H=240, Side=16, N=Side*Side, Cells=9;
// Point는 사진 위 점 하나입니다. 왼쪽 위가 (0,0)이며 오른쪽으로 x, 아래로 y가 커집니다.
struct Point { int x,y; };
struct Quad { Point p[4]; }; // 금속 보관판 바닥에서 1·3·9·7번 칸 바깥쪽에 있는 꼭짓점 순서

// 네 점이 사진 안에 있고 순서대로 이었을 때 충분한 넓이의 볼록 사각형이 되는지 확인합니다.
inline bool valid(const Quad &q) {
  for (Point p:q.p) {
    if (p.x<0 || p.x>=W || p.y<0 || p.y>=H) return false;
  }
  int sign=0;
  for (int i=0;i<4;++i) {
    Point a=q.p[i], b=q.p[(i+1)%4], c=q.p[(i+2)%4];
    int cross=(b.x-a.x)*(c.y-b.y)-(b.y-a.y)*(c.x-b.x);
    if (abs(cross)<900 || (sign && (cross>0)!=(sign>0))) return false;
    sign=cross;
  }
  return true;
}

// 판을 정면에서 본 가상 좌표를 실제 카메라 사진의 좌표로 옮깁니다(원근 변환).
inline void map(const Quad &q, uint32_t indices[Cells][N]) {
  const Point a=q.p[0], b=q.p[1], c=q.p[2], d=q.p[3];
  float dx=b.x-c.x, dy=b.y-c.y, ex=d.x-c.x, ey=d.y-c.y;
  float sx=a.x-b.x+c.x-d.x, sy=a.y-b.y+c.y-d.y;
// g와 h는 먼 쪽 칸이 사진에서 작게 보이는 정도를 반영합니다.
  float det=dx*ey-ex*dy, g=(sx*ey-ex*sy)/det, h=(dx*sy-sx*dy)/det;
  for(int k=0;k<Cells;++k) for(int j=0;j<N;++j) {
// k%3은 열, k/3은 행입니다(정수 나눗셈). 칸 양끝 7%를 빼고 가운데 86%에서 점을 고르면 금속 칸막이의 영향을 줄일 수 있습니다.
    float u=(k%3+0.07f+0.86f*(j%Side+0.5f)/Side)/3;
    float v=(k/3+0.07f+0.86f*(j/Side+0.5f)/Side)/3;
    float z=1+g*u+h*v;
    int x=lroundf((a.x+(b.x*(1+g)-a.x)*u+(d.x*(1+h)-a.x)*v)/z);
    int y=lroundf((a.y+(b.y*(1+g)-a.y)*u+(d.y*(1+h)-a.y)*v)/z);
// 예: 너비 320에서 (10,2)는 2×320+10=650번째 위치입니다. 사진을 찍을 때마다 이 표를 재사용합니다.
    indices[k][j]=y*W+x;
  }
}
// 16비트 색 하나에는 빨강 5비트·초록 6비트·파랑 5비트가 들어 있습니다. 비교할 색 성분만 꺼내 0~255로 바꿉니다.
inline int component(uint16_t px,int channel) {
  return channel==0 ? ((px>>11)*255/31) : channel==1 ? (((px>>5)&63)*255/63) : ((px&31)*255/31);
}
// 같은 위치의 빈 판과 현재 색을 비교하고, 서로 이웃한 점들이 함께 변한 면적을 계산합니다.
inline int score(const uint16_t *reference,const uint16_t *sample) {
  uint8_t mask[N]={};
  for(int i=0;i<N;++i) {
    int dr=component(sample[i],0)-component(reference[i],0);
    int dg=component(sample[i],1)-component(reference[i],1);
    int db=component(sample[i],2)-component(reference[i],2);
// dr,dg,db는 각 색의 변화량입니다. 밝기 변화 또는 색 성분 간 차이가 기준을 넘으면 이 점을 1로 표시합니다.
    mask[i]=(abs(dr+2*dg+db)>=4*34 || abs(dr-dg)>=42 || abs(db-dg)>=42);
  }
  int hits=0;
  for(int y=0;y<Side-1;++y) for(int x=0;x<Side-1;++x) {
    int i=y*Side+x;
// 이웃한 네 점 중 세 점 이상이 바뀐 곳을 한 번 셉니다. 15×15개 묶음 중 몇 묶음인지 천분율로 환산합니다.
    hits+=mask[i]+mask[i+1]+mask[i+Side]+mask[i+Side+1]>=3;
  }
  return (1000*hits)/((Side-1)*(Side-1));
}
constexpr int On=85, Off=55, Stable=3; // 변화율 단위는 천분율(‰). 85는 8.5%, 55는 5.5%입니다.
// 각 칸에는 occupied(현재 확정한 유무)와 streak(반대 상태 조건이 연속으로 맞은 횟수)가 있습니다.
struct Cell {
  bool occupied=false;
  uint8_t streak=0;
  void update(int value) {
// 빈 칸은 8.5% 이상, 차 있는 칸은 5.5% 이하일 때 전환 후보입니다. 3회 연속이어야 상태를 뒤집습니다.
    bool change=occupied ? value<=Off : value>=On;
    streak=change ? streak+1 : 0;
    if(streak>=Stable) { occupied=!occupied; streak=0; }
  }
};
}
