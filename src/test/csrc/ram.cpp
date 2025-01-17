#include "common.h"
#include "difftest.h"

#ifdef WITH_DRAMSIM3
#include "cosimulation.h"

CoDRAMsim3 *dram = NULL;
#endif

#define RAMSIZE (256 * 1024 * 1024)

static paddr_t ram[RAMSIZE / sizeof(paddr_t)];
static long img_size = 0;
void* get_img_start() { return &ram[0]; }
long get_img_size() { return img_size; }

void addpageSv39() {
//three layers
//addr range: 0x0000000080000000 - 0x0000000088000000 for 128MB from 2GB - 2GB128MB
//the first layer: one entry for 1GB. (512GB in total by 512 entries). need the 2th entries
//the second layer: one entry for 2MB. (1GB in total by 512 entries). need the 0th-63rd entries
//the third layer: one entry for 4KB (2MB in total by 512 entries). need 64 with each one all  

#define PAGESIZE (4 * 1024)  // 4KB = 2^12B
#define ENTRYNUM (PAGESIZE / 8) //512 2^9
#define PTEVOLUME (PAGESIZE * ENTRYNUM) // 2MB
#define PTENUM (RAMSIZE / PTEVOLUME) // 128MB / 2MB = 64
#define PDDENUM 1
#define PDENUM 1
#define PDDEADDR (0x88000000 - (PAGESIZE * (PTENUM + 2))) //0x88000000 - 0x1000*66
#define PDEADDR (0x88000000 - (PAGESIZE * (PTENUM + 1))) //0x88000000 - 0x1000*65
#define PTEADDR(i) (0x88000000 - (PAGESIZE * PTENUM) + (PAGESIZE * i)) //0x88000000 - 0x100*64
#define PTEMMIONUM 128
#define PDEMMIONUM 1

  uint64_t pdde[ENTRYNUM];
  uint64_t pde[ENTRYNUM];
  uint64_t pte[PTENUM][ENTRYNUM];
  
  //special addr for mmio 0x40000000 - 0x4fffffff
  uint64_t pdemmio[ENTRYNUM];
  uint64_t ptemmio[PTEMMIONUM][ENTRYNUM];
  
  pdde[1] = (((PDDEADDR-PAGESIZE*1) & 0xfffff000) >> 2) | 0x1;

  for(int i = 0; i < PTEMMIONUM; i++) {
    pdemmio[i] = (((PDDEADDR-PAGESIZE*(PTEMMIONUM+PDEMMIONUM-i)) & 0xfffff000) >> 2) | 0x1;
  }
  
  for(int outidx = 0; outidx < PTEMMIONUM; outidx++) {
    for(int inidx = 0; inidx < ENTRYNUM; inidx++) {
      ptemmio[outidx][inidx] = (((0x40000000 + outidx*PTEVOLUME + inidx*PAGESIZE) & 0xfffff000) >> 2) | 0xf;
    }
  }
  
  //0x800000000 - 0x87ffffff
  pdde[2] = ((PDEADDR & 0xfffff000) >> 2) | 0x1;
  //pdde[2] = ((0x80000000&0xc0000000) >> 2) | 0xf;

  for(int i = 0; i < PTENUM ;i++) {
    pde[i] = ((PTEADDR(i)&0xfffff000)>>2) | 0x1;
    //pde[i] = (((0x8000000+i*2*1024*1024)&0xffe00000)>>2) | 0xf;
  }

  for(int outidx = 0; outidx < PTENUM; outidx++ ) {
    for(int inidx = 0; inidx < ENTRYNUM; inidx++ ) {
      pte[outidx][inidx] = (((0x80000000 + outidx*PTEVOLUME + inidx*PAGESIZE) & 0xfffff000)>>2) | 0xf;
    }
  }

  memcpy((char *)ram+(RAMSIZE-PAGESIZE*(PTENUM+PDDENUM+PDENUM+PDEMMIONUM+PTEMMIONUM)),ptemmio, PAGESIZE*PTEMMIONUM);
  memcpy((char *)ram+(RAMSIZE-PAGESIZE*(PTENUM+PDDENUM+PDENUM+PDEMMIONUM)), pdemmio, PAGESIZE*PDEMMIONUM);
  memcpy((char *)ram+(RAMSIZE-PAGESIZE*(PTENUM+PDDENUM+PDENUM)), pdde, PAGESIZE*PDDENUM);
  memcpy((char *)ram+(RAMSIZE-PAGESIZE*(PTENUM+PDENUM)), pde, PAGESIZE*PDENUM);
  memcpy((char *)ram+(RAMSIZE-PAGESIZE*PTENUM), pte, PAGESIZE*PTENUM);
}

void init_ram(const char *img) {
  assert(img != NULL);
  FILE *fp = fopen(img, "rb");
  if (fp == NULL) {
    printf("Can not open '%s'\n", img);
    assert(0);
  }

  printf("The image is %s\n", img);

  fseek(fp, 0, SEEK_END);
  img_size = ftell(fp);
  if (img_size > RAMSIZE) {
    img_size = RAMSIZE;
  }

  fseek(fp, 0, SEEK_SET);
  int ret = fread(ram, img_size, 1, fp);
  assert(ret == 1);
  fclose(fp);

  //new add
  addpageSv39();
  //new end

#ifdef WITH_DRAMSIM3

#if !defined(DRAMSIM3_CONFIG) || !defined(DRAMSIM3_OUTDIR)
#error DRAMSIM3_CONFIG or DRAMSIM3_OUTDIR is not defined
#endif

  assert(dram == NULL);
  dram = new CoDRAMsim3(DRAMSIM3_CONFIG, DRAMSIM3_OUTDIR);

#endif

}

extern "C" void ram_helper(
    paddr_t rIdx, paddr_t *rdata, paddr_t wIdx, paddr_t wdata, paddr_t wmask, uint8_t wen) {
  *rdata = ram[rIdx];
  if (wen) { ram[wIdx] = (ram[wIdx] & ~wmask) | (wdata & wmask); }
}

#ifdef WITH_DRAMSIM3
#include <iostream>

void dramsim3_finish() {
  delete dram;
}

#define MAX_AXI_DATA_LEN 8

// currently does not support masked read or write
struct dramsim3_meta {
  uint8_t  len;
  uint8_t  offset;
  uint64_t data[MAX_AXI_DATA_LEN];
};

void axi_read_data(const axi_ar_channel &ar, dramsim3_meta *meta) {
  uint64_t address = ar.addr % RAMSIZE;
  uint64_t beatsize = 1 << ar.size;
  uint8_t  beatlen  = ar.len + 1;
  assert(ar.size == 3); // 2^3 = 8 bytes
  // axi burst FIXED
  if (ar.burst == 0x0) {
    std::cout << "arburst == FIXED not supported!" << std::endl;
    assert(0);
  }
  // axi burst INCR
  else if (ar.burst == 1) {
    assert(address % sizeof(uint64_t) == 0);
    for (int i = 0; i <= ar.len; i++) {
      meta->data[i] = ram[address / sizeof(uint64_t)];
      address += beatsize;
    }
  }
  // axi burst WRAP
  else if (ar.burst == 2) {
    uint64_t aligned = beatlen * beatsize;
    uint64_t low = (address / aligned) * aligned;
    uint64_t high = low + aligned;
    for (int i = 0; i <= ar.len; i++) {
      if (address == high) {
        address = low;
      }
      meta->data[i] = ram[address / sizeof(uint64_t)];
      address += beatsize;
    }
  }
  else {
    std::cout << "reserved arburst!" << std::endl;
    assert(0);
  }
  meta->len = beatlen;
  meta->offset = 0;
}

CoDRAMRequest *dramsim3_request(const axi_channel &axi, bool is_write) {
  uint64_t address = (is_write) ? axi.aw.addr : axi.ar.addr;
  dramsim3_meta *meta = new dramsim3_meta;
  // WRITE
  if (is_write) {
    meta->len = axi.aw.len + 1;
    meta->offset = 0;
  }
  else {
    axi_read_data(axi.ar, meta);
  }
  CoDRAMRequest *req = new CoDRAMRequest();
  req->address = address;
  req->is_write = is_write;
  req->meta = meta;
  return req;
}

void dramsim3_helper(axi_channel &axi) {
  // ticks DRAMsim3 according to CPU_FREQ:DRAM_FREQ
  dram->tick();

  static CoDRAMResponse *wait_resp_r = NULL;
  static CoDRAMResponse *wait_resp_b = NULL;
  static CoDRAMRequest *wait_req_w = NULL;
  // currently only accept one in-flight read + one in-flight write
  static uint64_t raddr, roffset = 0, rlen;
  static uint64_t waddr, woffset = 0, wlen;

  // default branch to avoid wrong handsha
  axi.aw.ready = 0;
  axi.w.ready  = 1;
  axi.b.valid  = 0;
  axi.ar.ready = 0;
  // axi.r.valid  = 0;

  // AXI read
  // first, check rdata in the last cycle
  if (axi.r.ready && axi.r.valid) {
    // printf("axi r channel fired\n");
    dramsim3_meta *meta = static_cast<dramsim3_meta *>(wait_resp_r->req->meta);
    meta->offset++;
    axi.r.valid = 0;
  }
  // second, check whether we response data in this cycle
  if (!wait_resp_r)
    wait_resp_r = dram->check_read_response();
  if (wait_resp_r) {
    dramsim3_meta *meta = static_cast<dramsim3_meta *>(wait_resp_r->req->meta);
    if (meta->offset == meta->len) {
      delete meta;
      delete wait_resp_r->req;
      delete wait_resp_r;
      wait_resp_r = NULL;
    }
    else {
      axi.r.data = meta->data[meta->offset];
      axi.r.valid = 1;
      axi.r.last = (meta->offset == meta->len - 1) ? 1 : 0;
    }
  }
  // third, check ar for next request's address
  // put ar in the last since it should be at least one-cycle latency
  if (axi.ar.valid && dram->will_accept(axi.ar.addr, false)) {
    dram->add_request(dramsim3_request(axi, false));
    axi.ar.ready = 1;
    // printf("axi ar channel fired\n");
  }

  // AXI write
  // first, check wdata in the last cycle
  // aw channel
  if (axi.aw.valid && dram->will_accept(axi.aw.addr, true)) {
    assert(wait_req_w == NULL); // the last request has not finished
    wait_req_w = dramsim3_request(axi, true);
    axi.aw.ready = 1;
    // printf("axi aw channel fired\n");
    assert(axi.aw.burst == 0 || (axi.aw.burst == 2 && ((axi.aw.addr & 0x3f) == 0)));
  }

  // w channel: ack write data
  if (axi.w.valid && axi.w.ready) {
    // printf("axi w channel fired\n");
    assert(wait_req_w);
    dramsim3_meta *meta = static_cast<dramsim3_meta *>(wait_req_w->meta);
    meta->data[meta->offset] = axi.w.data;
    meta->offset++;
    if (meta->offset == meta->len) {
      assert(dram->will_accept(wait_req_w->address, true));
      dram->add_request(wait_req_w);
      wait_req_w = NULL;
    }
  }

  // b channel: ack write
  if (!wait_resp_b)
    wait_resp_b = dram->check_write_response();
  if (wait_resp_b) {
    dramsim3_meta *meta = static_cast<dramsim3_meta *>(wait_resp_b->req->meta);
    axi.b.valid = 1;
    // assert(axi.b.ready == 1);
    for (int i = 0; i < meta->len; i++) {
      uint64_t address = wait_resp_b->req->address % RAMSIZE;
      ram[address / sizeof(uint64_t) + i] = meta->data[i];
    }
    // printf("axi b channel fired\n");
    delete meta;
    delete wait_resp_b->req;
    delete wait_resp_b;
    wait_resp_b = NULL;
  }
}

#endif
