// filnor.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <time.h>
#include <climits>

// Now managed by GIT

// Assuming 4M address space, 64 byte page size, 64k LUT entry

#define DWORD	long
#define BOOL	bool

using namespace		std;

long				cycle;				// global variable

#define MAX_ADDRESS	0x400000			// 4M word, assuming 64-bit QWord, that is 256Mbit

class CTrafficGen {

public:

	DWORD	address, waddress_o;
	int 	type; 						// 1-read, 2-write, 0-idle

public:

	CTrafficGen() {
		waddress_o = MAX_ADDRESS / 10;
	}

	DWORD	Address() {
		return address;
	}

	int Type() {
		return type;
	}

	void Update();					// Generate address and type
};

#define LUT_SIZE	0x10000			// 64k entries, entry 0 to 0xFFFF, a page is 64 QWORD

class	CLUT {

public:

	DWORD	page_index[LUT_SIZE];			// logic to physical page index translation table
	int		hit[LUT_SIZE];					// Entry in WB
	BOOL		in_flash[LUT_SIZE];				// Entry in Flash
	int		cur_pi;							// Last time GetHit is checked

public:

	CLUT();

//	void	Init();
	int		IsHit(DWORD);					// Check if in WB
	int		PageIndex();					// From the last WB check
	void	AddWBEntry(int);				// to the last WB check index
	int		RemoveWBEntry(int);				// Search pi and remove
	void	AddFlashEntry(int hpi, int fpi);		// With both logical and physical index specified
	int		RemoveFlashEntry(int);			// Search pi and remove, returns host index
	BOOL    IsInFlash() { return in_flash[cur_pi];}					// from cur logic pi 

	// check for cache hit, gets updated when new page allocated in WB.
};


#define	PAGE_PER_BLOCK	32		// 32 pages per block
#define PAGE_PER_BLOCK_BIT	5	// shift 5 bits

#define NUM_BLOCKS	0x800		// 2k blocks, need to add over provisioning later

class CBIR	{					// Block information repository

public:

	int		erasure_cnt[NUM_BLOCKS];					// Erasure count per block
	BOOL	in_fpq[NUM_BLOCKS];							// whether a block is in FPQ (fresh page queue)
	BOOL	bad_block[NUM_BLOCKS];						// Indication of a bad block during erase
	int		page_map[NUM_BLOCKS][PAGE_PER_BLOCK];		// 0: valid; 1 - invalid

public:

	CBIR();

	// Initial conditions: all blocks not erased. All page invalid. invalid cnt max. free page cnt is 0.
	// So the whole system start by erase blocks and build FPQ.

	int NumInvalidPages(int bi);	// Get number of invalid pages per block
	int SelectBlockGC();			// Select a block for GC
	int GetPiValid(int);			// Get the next valid page for GC write

	void MakeValid(int);
	void MakeInvalid(int);
};

#define FPQ_SIZE			129			// The actual useable unit is FPQ_SIZE - 1
#define FPQ_CNT_THRESHOLD	32

class	CFPQ		{			// Fresh page queue

public:

	int			tail, head;			// Tail and head of FPQ
	int			pi[FPQ_SIZE];		// The actuall queue that contains page index of fresh pages
	CBIR		*pBIR;				// A reference to BIR

public:

	CFPQ();

	int FPQItemCnt();		// Num of entries in Queue
	int PushBlock(int bi);	// 0 - OK, 1 - fail; Push a block of pages into the queue
	int PopPage();			// Pop a page out for write. Return page index; 

	void SetBIR(CBIR *pb) {
		pBIR = pb;
	}

	BOOL FpCntLow() {		// Indication of low FPQ entry count, triggers GC
		return (FPQItemCnt() < FPQ_CNT_THRESHOLD);
	}
};

#define WB_SIZE				0x100		// 256 pages, 0 to FF. Assuming 64 word pages, this is 1M bit memory, 16k word or 4000h word

#define WB_PG_SIZE			0x40		// 64 word per page
#define WB_PG_SIZE_BIT		6			// Shift 6 bit

class CWB {
	
	public:

	int credit[WB_SIZE];		// each page has a credit, the more page hit, the higher credit
	int busy_cnt;				// busy_cnt affects flush decision	

	BOOL busy[WB_SIZE];
	int flush_busy_counter;
	int	hit_cnt[WB_SIZE];

	int total_flush_hit;	

	//CWBPAGE page[WB_SIZE];

	CFPQ	*pFPQ;
	CLUT	*pLUT;
	CBIR	*pBIR;

public:

	CWB() {
		Init();
	}

	void SetFPQ(CFPQ *pf) {
		pFPQ = pf;
	}

	void SetLUT(CLUT *pl) {
		pLUT = pl;
	}

	void SetBIR(CBIR *pb) {
		pBIR = pb;
	}

	void Init();
	void WriteCache(int, int);
	int GetNewEntry();		// Searching for an un-occupied entry, if cache miss, 
	void Flush();			// Sort according to credit, then flush the one with lowest credit. Flush take some cycles.
	void Update();

	BOOL BusyFlush();
	BOOL BusyCntHigh();
};

class	CGC	{				// The Garbage Collector

public:

	CBIR	*pBIR;
	CLUT	*pLUT;
	CFPQ	*pFPQ;

	int cur_bi;				// From last search for GC block index
	int busy_cnt;			// Busy timer

public: 

	CGC() {
		cur_bi = -1;
		busy_cnt = 0;
	}

	void SetBIR(CBIR *pb) {
		pBIR = pb;
	}

	void SetLUT(CLUT *pl) {
		pLUT = pl;
	}

	void SetFPQ(CFPQ *pf) {
		pFPQ = pf;
	}

	BOOL isBusy() {
		return (busy_cnt > 0);
	}

	// To do:
	// SelectBlock from BIR
	// Get the pi from BIR
	// GC write
	// Update BIR and LUT
	// Loop

	// ...... 
//	int SelectBlock();

	void	GCWrite();		// 0: finished all invalid pages;	1: not finished
	void	GCErase();		// Return block number, update BIR

//	int PostGCErase();	// Stick Block to FPQ. 

	void Update() {
		if(busy_cnt > 0) busy_cnt--;
	}
};


BOOL page_hit_map[WB_SIZE][WB_PG_SIZE];

// #define SEQ

// #define RND

void CTrafficGen::Update() 
{
	int r;

	r = rand() % 3;

	if(!r) {
		address = 0;
		type = 0;
	}
	else if(r == 1) {
		address = 0;
		type = 1;
	}
	else {

#ifdef RND

//		long hb = rand();
//		long lb = rand();

//		long delta = hb * 100 + lb;			// 1M
//		address = delta;

		int b0 = rand() * 100;
		int b1 = rand() * 10;
		int b2 = rand();
		int b3 = rand() % 3000;
		int b4 = rand() % 300;
		int b5 = rand() % 30;

		address = b0+b2; // +b3+b4+b5;

#endif


#ifdef SEQ

		long base = cycle / 1000;
		base *= 100;
		long rd = rand() / 2;

		address = base + rd;
#endif

		/*
		long delta = rand() % 202 - 100;
		address = waddress_o + delta;

		if(address < 0) 
			address = rand() % 0x2000;
		else if(address > MAX_ADDRESS) 
			address = rand() % MAX_ADDRESS;
*/

		address = rand() % 0x3100;

		type = 2;

		waddress_o = address;
	}
}

int double_hit = 0;
int total_write = 0;

int WritePageCache(int pi, int addr)
{
	int hit = 0;
	
	if(!page_hit_map[pi][addr]) {
		page_hit_map[pi][addr] = true;
		hit = 1;
	}
	else {
		double_hit++;
	}

	total_write++;

	return hit;
}

void InitPageHitMap(int i)
{
	int j;

	for(j=0; j<WB_PG_SIZE; j++) {
			page_hit_map[i][j] = false;
	}
}

CLUT::CLUT()
{
	int i;

	cur_pi = 0;

	for(i=0; i<LUT_SIZE; i++) {
		page_index[i] = 0;
		hit[i] = 0;
		in_flash[i] = 0;
	}
}

int CLUT::IsHit(DWORD addr)
{
	DWORD i;

	i = addr >> WB_PG_SIZE_BIT;

	cur_pi = i;

	return hit[i];
}

void CLUT::AddWBEntry(int pi) 
{
	page_index[cur_pi] = pi;
	hit[cur_pi] = true;
}

void CLUT::AddFlashEntry(int hpi, int fpi) 
{
	page_index[hpi] = fpi;
	in_flash[hpi] = true;
}

int CLUT::RemoveWBEntry(int pi) 
{
	int i;

	for(i=0; i<LUT_SIZE; i++) {
		if(page_index[i] == pi) {
			hit[i] = 0;
			return i;
		}
	}

	return -1;
}

int CLUT::RemoveFlashEntry(int pi) 
{
	int i, hpi;

	for(i=0; i<LUT_SIZE; i++) {
		if(page_index[i] == pi) {
			hpi = i;
			in_flash[i] = false;
			break;
		}
	}

	return hpi;
}


int CLUT::PageIndex()
{
	// only if hit
	return page_index[cur_pi];
}

CBIR::CBIR()
{
	for(int i=0; i<NUM_BLOCKS; i++) {
		erasure_cnt[i] = 0;
		in_fpq[i] = false;
		bad_block[i] = false;

		for(int j=0; j<PAGE_PER_BLOCK; j++) {
			page_map[i][j] = 1;
		}
	}
}

int CBIR::NumInvalidPages(int bi)
{
	int n = 0;

	for(int i=0; i<PAGE_PER_BLOCK; i++) {
		if(page_map[bi][i]) n++;
	}

	return n;
}

int CBIR::SelectBlockGC()
{
	int nivp, ec, big_nivp, big_ec;
	int big_bi;

	for(int i=0; i<NUM_BLOCKS; i++) {
		nivp = NumInvalidPages(i);
		if(!in_fpq[i]) {				// Find first not in FPQ
			big_bi = i;
			big_nivp = nivp;
			big_ec = erasure_cnt[i];
			break;
		}
	}

	for(int i=big_bi + 1; i<NUM_BLOCKS; i++) {
		nivp = NumInvalidPages(i);
		ec = erasure_cnt[i];
		if(!in_fpq[i]) {				// Exclude blocks that are in FPQ
			if(nivp > big_nivp) {
				big_bi = i;
				big_nivp = nivp;
				big_ec = erasure_cnt[i];
			}
			else if(nivp = big_nivp) {		// if valide page same, but erase count smaller, replace too
				if(ec < big_ec) {
					big_bi = i;
					big_nivp = nivp;
					big_ec = erasure_cnt[i];
				}
			}
		}
	}

	return big_bi;
}

int CBIR::GetPiValid(int bi)
{
	for(int i=0; i<PAGE_PER_BLOCK; i++) {
		if(!page_map[bi][i]) return bi * PAGE_PER_BLOCK + i;
	}

	return -1;
}

void CBIR::MakeValid(int pi)
{
	int bi = pi / PAGE_PER_BLOCK;
	int i = pi % PAGE_PER_BLOCK;

	page_map[bi][i] = 0;
}

void CBIR::MakeInvalid(int pi)
{
	int bi = pi / PAGE_PER_BLOCK;
	int i = pi % PAGE_PER_BLOCK;

	page_map[bi][i] = 1;
}

CFPQ::CFPQ()
{
	head = 0;
	tail = 0;

	for(int i=0; i<FPQ_SIZE; i++) {
		pi[i] = 0;
	}
}

int CFPQ::FPQItemCnt()
{
	if(tail >= head) {
		return tail - head;
	}
	else {
		return tail + FPQ_SIZE - head;
	}
}

int CFPQ::PushBlock(int bi)
{
	if(FPQItemCnt() > FPQ_SIZE - PAGE_PER_BLOCK) return 1;	// not enough space in the queue

	for (int i = 0; i<PAGE_PER_BLOCK; i++) {
		int tailplus = tail + 1;

		pi[tail] = bi * PAGE_PER_BLOCK + i;

		if(tailplus >= FPQ_SIZE) tail = tailplus - FPQ_SIZE;
		else tail = tailplus;
	}

	pBIR->in_fpq[bi] = true;

	return 0;
}

int CFPQ::PopPage()
{
	int my_pi = pi[head];

	if(head + 1 >= FPQ_SIZE) head = head + 1 - FPQ_SIZE;
	else head = head + 1;

	// To do: check if still in FPQ

	int rmd = my_pi % PAGE_PER_BLOCK;
	
	if(rmd == PAGE_PER_BLOCK - 1) {				// Just poped out the last page
		int bi = my_pi / PAGE_PER_BLOCK;
		pBIR->in_fpq[bi] = false;
	}

	return my_pi;
}

void CGC::GCWrite()
{
	int bi = pBIR->SelectBlockGC();
	cur_bi = bi;
	
	int pi = pBIR->GetPiValid(bi);			

	while(pi > 0) {		// Cycle through all valid pages for GC write

		int fpi = pFPQ->PopPage();		// Get a fresh page
		
		// Copy page here...

		// Update LUT entry
		int hpi = pLUT->RemoveFlashEntry(pi);
		pLUT->AddFlashEntry(hpi, fpi);

		busy_cnt += 40;							// 40 cycles

		pBIR->MakeInvalid(pi);
		pBIR->MakeValid(fpi);

		pi = pBIR->GetPiValid(bi);				// Get the next valid page
	}
}

void CGC::GCErase()
{
	GCWrite();

	int bi = cur_bi;		// GCErase always follows GCWrite

	// Perform block erase

	busy_cnt += 1000;			// 1000 cycles
	
	pBIR->erasure_cnt[bi]++;

	pFPQ->PushBlock(bi);		// Push the block into the FPQ 
}
	
void CWB::Init()
{
	int i;

	busy_cnt = 0;

	for(i=0; i<WB_SIZE; i++) {
		busy[i] = false;
		credit[i] = 0;
		hit_cnt[i] = 0;
	}

	flush_busy_counter = 0;
	total_flush_hit = 0;
}

// Get a free entry in case of cache miss, what if there is none.

int CWB::GetNewEntry()
{
	int i;

	for(i=0; i<WB_SIZE; i++) {
		if(!busy[i]) {
			busy[i] = true;
//			credit[i] = 0;
			return i;
		}
	}

	return WB_SIZE + 10;	// No new entry
}

void CWB::WriteCache(int pi, int addr)
{
	int hit = 0;

	// To do: Copy flash content to WB page and invalid that page in flash.

	hit = WritePageCache(pi, addr);

	credit[pi] += 3000;

	if(credit[pi] > INT_MAX) credit[pi] = INT_MAX;

	if(hit) hit_cnt[pi]++;
}

void CWB::Update()
{
	int i, cnt=0;

	for(i=0; i<WB_SIZE; i++) {
		if(busy[i]) cnt++;
		if(credit[i] > 0) credit[i]--;
	}

	busy_cnt = cnt;

	if(flush_busy_counter > 0) flush_busy_counter--;
}

void CWB::Flush()
{
	int i, min, min_i;

	// Search the page with min credit for flush

	for(i=0; i<WB_SIZE; i++) {			// find initial min and min_i
		if(busy[i]) {
			min = credit[i];
			min_i = i;
			break;
		}
	}

	for(i=min_i+1; i<WB_SIZE; i++) {		// update min and min_i
		if(credit[i] < min && busy[i]) {
			min = credit[i];
			min_i = i;
		}
	}

	// ... then flush min_i entry;

	total_flush_hit += hit_cnt[min_i];

	busy[min_i] = false;
	credit[min_i] = 0;
	hit_cnt[min_i] = 0;
	InitPageHitMap(min_i);

	// Get fresh page

	int fpi = pFPQ->PopPage();

	// Update LUT

	//	Remove entry min_i
	//	Add entry fpi

	int hpi = pLUT->RemoveWBEntry(min_i);
	pLUT->AddFlashEntry(hpi, fpi);

	//pBIR->page_map[fpi/PAGE_PER_BLOCK][fpi%PAGE_PER_BLOCK] = 0;		// Page is valid.

	pBIR->MakeValid(fpi);

	flush_busy_counter = 40;			// Takes 40 cycle to flush
}

BOOL CWB::BusyFlush()
{
	return (flush_busy_counter > 0);
}

BOOL CWB::BusyCntHigh()
{
	return busy_cnt > WB_SIZE * 3 / 4;			// 75% 
}


/*

Main flow:

1. CTrafficGen generates a cycle with address:
2. Check if hit or not

If Hit:

3. Write to cache, update credit

If Miss:

4. Get Cache entry (if no entry, re-try at cost of more cycles). 
5. Update LUT
6. Write to cache, initial credit, set busy flag.

Periodic flush and gc:

If not busy_flush or busy GC - meaning flash is not occupied
If FP cnt low, GC
If Busy Cnt is high: Find entry with least credit to flush. Update busy flag and LUT entry.
Otherwise: GC any way (Maybe use a timer)

*/

int _tmain(int argc, _TCHAR* argv[])
{
	CTrafficGen 	tg;

	CLUT			lut;	
	CBIR			bir;
	CFPQ			fpq;

	CWB				wb;
	CGC				gc;

	int				pi;			// page index;
	int				i;
	int				stalled = 0;

	int				hit = 0;
	int				type = 0;

// run statitics

	double hit_rate = 0;

	int	flush = 0, flush_pi, flush_cnt = 0;

	int isgc = 0, gc_cnt = 0;

	int total_cnt = 55000; // 15000000;

	double w_atten = 0; 

	for(i=0; i<WB_SIZE; i++) {
		InitPageHitMap(i);
	}

//=============

	DWORD waddr = 0;

	fpq.SetBIR(&bir);

	wb.SetBIR(&bir);
	wb.SetFPQ(&fpq);
	wb.SetLUT(&lut);

	gc.SetBIR(&bir);
	gc.SetFPQ(&fpq);
	gc.SetLUT(&lut);

	for(i=0; i<total_cnt; i++)		// 100,000 cycles, assuming 10ns cycle, this is 1ms.
	{
		if(!stalled) tg.Update();

		type = tg.Type();

		if(type == 2) {				// if it is write
			waddr = tg.Address();

			if(lut.IsHit(waddr)) {	// in WB
				hit = 1;
				pi = lut.PageIndex();	// Page index in WB
				wb.WriteCache(pi, waddr % WB_PG_SIZE);
			}
			else {						// not in WB
				hit = 0;

				// Is it in flash? If so what is the flash pi.

				int wb_pi = wb.GetNewEntry();	// New Page index in WB

				if(wb_pi > WB_SIZE) {
					stalled = true;				// don't do anything
				}
				else {
					stalled = false;

					// If in flash, need to copy content to cache and invalidate that page.
					if(lut.IsInFlash()) {
						pi = lut.PageIndex();		// Page index in Flash
						bir.MakeInvalid(pi);
						lut.in_flash[lut.cur_pi] = false;
					}

					
					wb.WriteCache(wb_pi,  waddr % WB_PG_SIZE);
					lut.AddWBEntry(wb_pi);		// at the same host_pi location
				}
			}
		}

		hit_rate += 0.01 * ((double)hit - hit_rate);

		if(!wb.BusyFlush() && !gc.isBusy()) {
			if(fpq.FpCntLow()) {
				gc.GCErase();				// GC has higher priority if FpCntLow
				isgc = 1;
				gc_cnt++;
			}
			else if(wb.BusyCntHigh()) {
				wb.Flush();		// pi: Page index in WB selected for flush
				flush = 1;
//				flush_pi = pi;
				flush_cnt++;
			}
//			else {						// Set a timer such that GC does not happen too often
//				gc.GC....
//			}
		}
		else {
			flush = 0;
			isgc = 0;
		}

		// Currently write and erase finishes immediately. But there is a delay before next write or erase can happen. 
		// This needs to be updated to relfect reality - that action is finished after, not before the time delay.

		wb.Update();
		gc.Update();

		if(wb.total_flush_hit > 0) w_atten = (double)total_write / (double)wb.total_flush_hit;

		if(type < 2 || stalled) {
//			cout << "cycle: " << cycle;
		}
		else if( /*cycle > total_cnt - 1000 || 1*/ cycle % 10 == 0) {
			cout << "cycle: " << cycle << " type: " << tg.Type() << " addr: " << waddr << " hit: " << hit_rate << " wb busy cnt: " << wb.busy_cnt << " flush_cnt: " << flush_cnt << " w_atten: " << w_atten << " double hit: " << double_hit << " total_write: " << wb.total_flush_hit << " gc_cnt: " << gc_cnt << " fpq cnt: " << fpq.FPQItemCnt();
			cout << endl;
		}

		cycle++;
	}
	cout << "Total flush hit:" << wb.total_flush_hit << " total double hit: " << double_hit << endl;
		if(wb.total_flush_hit > 0) cout << "Write attenuation:" << (double)cycle / ((double) wb.total_flush_hit * 3) << endl;

//	int dummy;
//	cout << "Enter a number to finish...";
//	cin >> dummy;

	return 0;
}

