/*
 * dbvmoffload.c
 *
 * port of the windows offload of CE's driver
 *
 *  Created on: Nov 21, 2017
 *      Author: eric
 */
#include "dbvmoffload.h"
#include "helpers.h"

typedef UINT64 UINT_PTR, *PUINT64, *PUINT_PTR;
typedef UINT32 DWORD, *PDWORD;
typedef UINT16 WORD, *PWORD;
typedef void *PVOID;



extern void enterVMM( void ); //declared in vmxoffloada.asm
extern void enterVMMPrologue(void);
extern void enterVMMEpilogue(void);
extern int doSystemTest(void);

#pragma pack(2)
struct
{
  WORD limit;
  UINT_PTR base;
} NewGDTDescriptor;
#pragma pack()

#pragma pack(1)
typedef struct
{ //ok, everything uint64, I hate these incompatibilities with alignment between gcc and ms c
  UINT64    cpucount;
  UINT64    originalEFER;
  UINT64    originalLME;
  UINT64    idtbase;
  UINT64    idtlimit;
  UINT64    gdtbase;
  UINT64    gdtlimit;
  UINT64    cr0;
  UINT64    cr2;
  UINT64    cr3;
  UINT64    cr4;
  UINT64    dr7;
  UINT64    rip;

  UINT64    rax;
  UINT64    rbx;
  UINT64    rcx;
  UINT64    rdx;
  UINT64    rsi;
  UINT64    rdi;
  UINT64    rbp;
  UINT64    rsp;
  UINT64    r8;
  UINT64    r9;
  UINT64    r10;
  UINT64    r11;
  UINT64    r12;
  UINT64    r13;
  UINT64    r14;
  UINT64    r15;

  UINT64    rflags;
  UINT64    cs;
  UINT64    ss;
  UINT64    ds;
  UINT64    es;
  UINT64    fs;
  UINT64    gs;
  UINT64    tr;
  UINT64    ldt;

  UINT64    cs_AccessRights;
  UINT64    ss_AccessRights;
  UINT64    ds_AccessRights;
  UINT64    es_AccessRights;
  UINT64    fs_AccessRights;
  UINT64    gs_AccessRights;

  UINT64    cs_Limit;
  UINT64    ss_Limit;
  UINT64    ds_Limit;
  UINT64    es_Limit;
  UINT64    fs_Limit;
  UINT64    gs_Limit;

  UINT64    fsbase;
  UINT64    gsbase;
  UINT64    APEntryPage; //Physical address in the lower 1MB memory used for AP bootup
  UINT64    Uncached;
} OriginalState, *POriginalState;

typedef struct
{
  UINT64 startAddress;
  UINT64 byteSize;
} __attribute__((__packed__)) UncachedRegion, *PUncachedRegion;

#pragma pack()

POriginalState originalstate;
UINT_PTR pagedirptrbasePA;
UINT_PTR NewGDTDescriptorVA;

unsigned char *enterVMM2;


UINT_PTR enterVMM2PA;

PVOID TemporaryPagingSetup;
UINT_PTR TemporaryPagingSetupPA;
UINT_PTR originalstatePA;
UINT_PTR NewGDTDescriptorVA;
UINT_PTR InitStackPA;
UINT_PTR vmmPA;

int initializedvmm=0;




void InitializeDBVM(UINT64 vmm, int vmmsize)
{
  //basic paging setup for the vmm, will get expanded by the vmm itself
  UINT64 FreePA=((vmm+vmmsize) & 0xfffffffffffff00ULL) +4096;
  UINT64 mainstack=FreePA;
  FreePA+=16*4096;
  UINT64 *GDTBase=(UINT64 *)((UINT_PTR)vmm+vmmsize+4096);
  FreePA+=4096;
  PUINT64  pagedirptrbase=(PUINT64)FreePA;
  FreePA+=4096;
  PUINT64  PageDirPtr=(PUINT64)(PageDirPtr+4096);
  FreePA+=4096;
  PUINT64  PageDir=(PUINT64)(PageDirPtr+4096);
  FreePA+=4096;
  PUINT64  PageMapLevel4=(PUINT64)PageDir; //must be the last alloc
  FreePA+=4096;

  ZeroMem((void*)mainstack, FreePA-mainstack); //initialize all the allocated memory


  Print(L"pagedirptrbase=%lx\n",pagedirptrbase);
  pagedirptrbasePA=(UINT_PTR)pagedirptrbase;

  Print(L"Setting up initial paging table for vmm\n");

  PageMapLevel4[0]=(UINT_PTR)PageDirPtr;
  ((PPDPTE_PAE)(&PageMapLevel4[0]))->P=1;
  ((PPDPTE_PAE)(&PageMapLevel4[0]))->RW=1;

  Print(L"PageMapLevel4[0]=%lx\n",PageMapLevel4[0]);


  PageDirPtr[0]=(UINT_PTR)PageDir;
  ((PPDPTE_PAE)(&PageDirPtr[0]))->P=1;
  ((PPDPTE_PAE)(&PageDirPtr[0]))->RW=1;
  Print(L"PageDirPtr[0]=%lx\n",PageDirPtr[0]);

  PageDir[0]=0; //physical address 0x00000000 to 0x00200000
  ((PPDE2MB_PAE)(&PageDir[0]))->P=1;
  ((PPDE2MB_PAE)(&PageDir[0]))->US=1;
  ((PPDE2MB_PAE)(&PageDir[0]))->RW=1;
  ((PPDE2MB_PAE)(&PageDir[0]))->PS=1; //2MB
  Print(L"PageDir[0]=%lx\n",PageDir[0]);

  PageDir[1]=0x00200000; //physical address 0x00200000 to 0x00400000
  ((PPDE2MB_PAE)(&PageDir[1]))->P=1;
  ((PPDE2MB_PAE)(&PageDir[1]))->US=1;
  ((PPDE2MB_PAE)(&PageDir[1]))->RW=1;
  ((PPDE2MB_PAE)(&PageDir[1]))->PS=1; //2MB
  Print(L"PageDir[1]=%lx\n",PageDir[1]);

  {
    //create a pagetable for the first 2MB of vmm
    PUINT64 PageTable=AllocatePersistentMemory(4096);
    int i;

    PageDir[2]=(UINT64)PageTable;
    ((PPDE2MB_PAE)(&PageDir[2]))->P=1;
    ((PPDE2MB_PAE)(&PageDir[2]))->RW=1;
    ((PPDE2MB_PAE)(&PageDir[2]))->PS=0; //points to a pagetable

    Print(L"PageDir[2]=%lx\n",PageDir[2]);

    //fill in the pagetable
    for (i=0; i<512; i++)
    {
      PageTable[i]=(UINT_PTR)vmm+(4096*i);
      ((PPTE_PAE)(&PageTable[i]))->P=1;
      ((PPTE_PAE)(&PageTable[i]))->RW=1;
    }

    if (mainstack<vmm+0x00200000)
    {
      int index=(mainstack-vmm) >> 12;
      ((PPTE_PAE)(&PageTable[index]))->P=0; //mark the first page of the stack as unreadable
    }
  }

  {
     //create a pagetable for the 2nd 2MB of vmm
     PUINT64 PageTable=AllocatePersistentMemory(4096);
     int i;

     PageDir[3]=(UINT64)PageTable;
     ((PPDE2MB_PAE)(&PageDir[3]))->P=1;
     ((PPDE2MB_PAE)(&PageDir[3]))->RW=1;
     ((PPDE2MB_PAE)(&PageDir[3]))->PS=0;

     Print(L"PageDir[3]=%lx\n",PageDir[3]);

     //fill in the pagetable
     for (i=0; i<512; i++)
     {
       PageTable[i]=(UINT_PTR)vmm+0x00200000+(4096*i);
       ((PPTE_PAE)(&PageTable[i]))->P=1;
       ((PPTE_PAE)(&PageTable[i]))->RW=1;
     }
   }



  //setup GDT
  GDTBase[0 ]=0;            //0 :
  GDTBase[1 ]=0x00cf92000000ffffULL;  //8 : 32-bit data
  GDTBase[2 ]=0x00cf96000000ffffULL;  //16: test, stack, failed, unused
  GDTBase[3 ]=0x00cf9b000000ffffULL;  //24: 32-bit code
  GDTBase[4 ]=0x00009a000000ffffULL;  //32: 16-bit code
  GDTBase[5 ]=0x000092000000ffffULL;  //40: 16-bit data
  GDTBase[6 ]=0x00009a030000ffffULL;  //48: 16-bit code, starting at 0x30000
  GDTBase[7 ]=0;            //56: 32-bit task
  GDTBase[8 ]=0;            //64: 64-bit task
  GDTBase[9 ]=0;            //72:  ^   ^   ^
  //GDTBase[10]=0x00a09e0000000000ULL;  //80: 64-bit code
  GDTBase[10]=0x00af9b000000ffffULL;  //80: 64-bit code
  GDTBase[11]=0;            //88:  ^   ^   ^
  GDTBase[12]=0;            //96: 64-bit tss descriptor (2)
  GDTBase[13]=0;            //104: ^   ^   ^


  NewGDTDescriptor.limit=0x6f; //111
  NewGDTDescriptor.base=0x00400000+vmmsize+4096; //virtual address to the gdt

  Print(L"&NewGDTDescriptor=%lx, &NewGDTDescriptor.limit=%lx, &NewGDTDescriptor.base=%lx\n",(UINT64)&NewGDTDescriptor,(UINT64)&NewGDTDescriptor.limit, (UINT64)&NewGDTDescriptor.base);
  Print(L"NewGDTDescriptor.limit=%x\n",NewGDTDescriptor.limit);
  Print(L"NewGDTDescriptor.base=%lx\n",(UINT64)NewGDTDescriptor.base);

  NewGDTDescriptorVA=(UINT_PTR)&NewGDTDescriptor;

  /*
   * AllocatePages
   */
  {
    EFI_PHYSICAL_ADDRESS address=0x003fffff;
    EFI_STATUS s;

    address=0x003fffffULL; //allocate 4k at the lower 4MB
    Print(L"Before enterVMM2 alloc: maxPA=%lx, x\n", address);

    s=AllocatePages(AllocateMaxAddress,EfiRuntimeServicesCode, 1,&address);
    if (s!=EFI_SUCCESS)
    {
      Print(L"Failure allocating low memory region");
      return;
    }

    ZeroMem((void *)address,4096);

    enterVMM2=(unsigned char *)address;

    //unsigned char *original=(unsigned char *)enterVMM;
    ZeroMem(enterVMM2,4096);
    Print(L"enterVMM is located at %lx\n", (UINT64)enterVMM);
    Print(L"enterVMM2 is located at %lx\n", (UINT64)enterVMM2);

    Print(L"Copying function till end\n");
    {
      int i;
      unsigned char *original=(unsigned char *)enterVMM;
      i=0;
      while ((i<4096) && ((original[i]!=0xce) || (original[i+1]!=0xce) || (original[i+2]!=0xce) || (original[i+3]!=0xce) || (original[i+4]!=0xce)))
        i++;

      Print(L"size is %d\n",i);

      CopyMem(enterVMM2, original, i);

      Print(L"copy done\n");

    }

  }

  //skip the setting up of identity mapped pages, because we're at that atm

  enterVMM2PA=(UINT_PTR)enterVMM2;
  originalstate=AllocatePersistentMemory(sizeof(OriginalState));
  ZeroMem(originalstate, sizeof(OriginalState));
  originalstatePA=(UINT_PTR)originalstate;
  Print(L"enterVMM2PA=%lx\n",enterVMM2PA);
  Print(L"originalstatePA=%lx\n",originalstatePA);

  //setup init vars
  {
    EFI_PHYSICAL_ADDRESS address=0x003fffff;
    EFI_STATUS s;

    UINT64 *vmimg=(UINT64 *)vmm;
    vmimg[2]=originalstatePA;
    vmimg[3]=vmmPA;
    vmimg[4]=0x00400000+(PageMapLevel4-vmm); //address of the PML4 Virtual address. 4096 bytes after that will be free for use
    vmimg[5]=0x00400000+(mainstack-vmm)+(16*4096)-0x40;

    address=0;
    s=AllocatePages(AllocateAnyPages,EfiRuntimeServicesData, 16384,&address); //64MB of memory
    if (s!=EFI_SUCCESS)
    {
      Print(L"Allocated 64MB of extra ram at %lx\n", address);
      vmimg[6]=address;
      vmimg[7]=16384;
    }
    else
    {
      vmimg[6]=0;
      vmimg[7]=0;
    }




    address=0xfffff;
    s=AllocatePages(AllocateMaxAddress,EfiRuntimeServicesCode, 1,&address);
    if (s!=EFI_SUCCESS)
    {
      Print(L"Failure allocating under 1MB region for AP launch");
      return;
    }
    originalstate->APEntryPage=address;

    //get the uncached memory regions

    UINTN descriptorbuffersize=sizeof(EFI_MEMORY_DESCRIPTOR)*64;
    EFI_MEMORY_DESCRIPTOR *descriptors=AllocatePool(descriptorbuffersize);
    UINTN MapKey;
    UINTN DescriptorSize;
    UINTN DescriptorVersion;
    int loop=1;

    while (uefi_call_wrapper(st->BootServices->GetMemoryMap, 5, &descriptorbuffersize, descriptors, &MapKey, &DescriptorSize, &DescriptorVersion)==EFI_BUFFER_TOO_SMALL)
    {
      //reallocate
      FreePool(descriptors);
      descriptorbuffersize=descriptorbuffersize+DescriptorSize*loop*4;
      descriptors=AllocatePool(descriptorbuffersize);
      if (descriptors)
      {
        loop++;

        if (loop>100)
        {
          Print(L"Giving up allocating memory for MemoryMap\n");
        }
      }
      else
        Print(L"Failure allocating memory for MemoryMap\n");
    }

    Print(L"descriptorbuffersize=%d bytes\n", descriptorbuffersize);
    Print(L"DescriptorVersion=%d\n", DescriptorVersion);

   // asm volatile (".byte 0xf1");
    /*

    PUncachedRegion uncachedmap=AllocatePersistentMemory(4096);
    int uncachedmapSize=0;
    ZeroMem(uncachedmap,4096);

    originalstate->Uncached=(UINT64)uncachedmap;

    {
      int i=0;//,j=0;
      unsigned char *buffer=(unsigned char *)descriptors;
      while (i<descriptorbuffersize)
      {
        EFI_MEMORY_DESCRIPTOR *desc=(EFI_MEMORY_DESCRIPTOR *)&buffer[i];

        if (((desc->Attribute & 0xf)!=0xf) && (desc->NumberOfPages))
        {
          Print(L"%d: PS=%lx->%lx VS=%lx Type=%d Attribute=%lx\n", (int)uncachedmapSize, (UINT64)desc->PhysicalStart, (UINT64)desc->PhysicalStart+desc->NumberOfPages*4096, (UINT64)desc->VirtualStart, (int)desc->Type, (UINT64)desc->Attribute);

          //find insert point
          int inserted=0;
          int j;
          for (j=0; j<uncachedmapSize; j++)
          {
            if (uncachedmap[j].startAddress>desc->PhysicalStart)
            {
              //insert in front of this
              int k;

              //first shift all the items after this one spot
              //find the end of the list
              for (k=uncachedmapSize; k>j; k--)
                uncachedmap[k]=uncachedmap[k-1];

              uncachedmap[j].startAddress=desc->PhysicalStart;
              uncachedmap[j].byteSize=desc->NumberOfPages*4096;
              inserted=1;
              uncachedmapSize++;
              break;
            }
          }

          if (inserted==0)
          {
            uncachedmap[uncachedmapSize].startAddress=desc->PhysicalStart;
            uncachedmap[uncachedmapSize].byteSize=desc->NumberOfPages*4096;

            uncachedmapSize++;
          }
        }

        i+=DescriptorSize;
      }

      Print(L" ----- \n");
      for (i=0; uncachedmap[i].byteSize; i++)
      {
        Print(L"%d: PS=%lx->%lx\n", (int)i, (UINT64)uncachedmap[i].startAddress, (UINT64)uncachedmap[i].startAddress+uncachedmap[i].byteSize);
      }
    }
    FreePool(descriptors);
*/



  }
  //WCHAR something[200];
  //Input(L"Type something : ", something, 200);
  //Print(L"\n");

  initializedvmm=TRUE;


}

void LaunchDBVM()
{
   GDT gdt;
   IDT idt;
   EFLAGS eflags;
   CHAR16 something[200];

   if (!initializedvmm)
   {
     Print(L"First initialize DBVM\n");
     return;
   }

   Print(L"Storing original state\n");
   originalstate->cpucount=0;  //indicate that dbvm needs to initialize the secondary CPU's
   Print(L"originalstate->cpucount=%d",originalstate->cpucount);

   originalstate->originalEFER=readMSR(0xc0000080); //amd prefers this over an LME

   originalstate->originalLME=(int)(((DWORD)(readMSR(0xc0000080)) >> 8) & 1);
   Print(L"originalstate->originalLME=%d\n",originalstate->originalLME);

   originalstate->cr0=(UINT_PTR)getCR0();
   Print(L"originalstate->cr0=%lx\n",originalstate->cr0);

   originalstate->cr2=(UINT_PTR)getCR2();
   Print(L"originalstate->cr2=%lx\n",originalstate->cr2);

   originalstate->cr3=(UINT_PTR)getCR3();
   Print(L"originalstate->cr3=%lx\n",originalstate->cr3);

   originalstate->cr4=(UINT_PTR)getCR4();
   Print(L"originalstate->cr4=%lx\n",originalstate->cr4);

   originalstate->ss=getSS();
   originalstate->ss_AccessRights=getAccessRights(originalstate->ss);
   originalstate->ss_Limit=getSegmentLimit(originalstate->ss);
   Print(L"originalstate->ss=%lx (%x:%x)\n",originalstate->ss, originalstate->ss_AccessRights, originalstate->ss_Limit);

   originalstate->cs=getCS();
   originalstate->cs_AccessRights=getAccessRights(originalstate->cs);
   originalstate->cs_Limit=getSegmentLimit(originalstate->cs);
   Print(L"originalstate->cs=%lx (%x:%x)\n",originalstate->cs, originalstate->cs_AccessRights, originalstate->cs_Limit);
   originalstate->ds=getDS();
   originalstate->ds_AccessRights=getAccessRights(originalstate->ds);
   originalstate->ds_Limit=getSegmentLimit(originalstate->ds);
   Print(L"originalstate->ds=%lx (%x:%x)\n",originalstate->ds, originalstate->ds_AccessRights, originalstate->ds_Limit);
   originalstate->es=getES();
   originalstate->es_AccessRights=getAccessRights(originalstate->es);
   originalstate->es_Limit=getSegmentLimit(originalstate->es);
   Print(L"originalstate->es=%lx (%x:%x)\n",originalstate->es, originalstate->es_AccessRights, originalstate->es_Limit);
   originalstate->fs=getFS();
   originalstate->fs_AccessRights=getAccessRights(originalstate->fs);
   originalstate->fs_Limit=getSegmentLimit(originalstate->fs);
   Print(L"originalstate->fs=%lx (%x:%x)\n",originalstate->fs, originalstate->fs_AccessRights, originalstate->fs_Limit);
   originalstate->gs=getGS();
   originalstate->gs_AccessRights=getAccessRights(originalstate->gs);
   originalstate->gs_Limit=getSegmentLimit(originalstate->gs);
   Print(L"originalstate->gs=%lx (%x:%x)\n",originalstate->gs, originalstate->gs_AccessRights, originalstate->gs_Limit);
   originalstate->ldt=getLDT();
   Print(L"originalstate->ldt=%lx\n",originalstate->ldt);
   originalstate->tr=getTR();
   Print(L"originalstate->tr=%lx\n",originalstate->tr);

   originalstate->fsbase=readMSR(0xc0000100);
   originalstate->gsbase=readMSR(0xc0000101);


   Print(L"originalstate->fsbase=%lx originalstate->gsbase=%lx\n", originalstate->fsbase, originalstate->gsbase);

   originalstate->dr7=getDR7();

   gdt.vector=0;
   gdt.wLimit=0;
   getGDT(&gdt);

   originalstate->gdtbase=(UINT64)gdt.vector;
   originalstate->gdtlimit=gdt.wLimit;

   Print(L"originalstate->gdtbase=%lx\n",originalstate->gdtbase);
   Print(L"originalstate->gdtlimit=%lx\n",originalstate->gdtlimit);

   getIDT(&idt);
   originalstate->idtbase=(UINT64)idt.vector;
   originalstate->idtlimit=idt.wLimit;

   Print(L"originalstate->idtbase=%lx\n",originalstate->idtbase);
   Print(L"originalstate->idtlimit=%lx\n",originalstate->idtlimit);

   eflags=getEflags();
   originalstate->rflags=*(PUINT_PTR)&eflags;
   Print(L"originalstate->rflags was %lx\n",(UINT64)originalstate->rflags);

   eflags.IF=0;
   originalstate->rflags=*(PUINT_PTR)&eflags;

   Print(L"originalstate->rflags is %lx\n",(UINT64)originalstate->rflags);

   originalstate->rflags=*(PUINT_PTR)&eflags;

   originalstate->rsp=getRSP();
   Print(L"originalstate->rsp=%lx\n",originalstate->rsp);
   originalstate->rbp=getRBP();
   Print(L"originalstate->rbp=%lx\n",originalstate->rbp);


   originalstate->rax=getRAX();
   Print(L"originalstate->rax=%lx\n",originalstate->rax);
   originalstate->rbx=getRBX();
   Print(L"originalstate->rbx=%lx\n",originalstate->rbx);
   originalstate->rcx=getRCX();
   Print(L"originalstate->rcx=%lx\n",originalstate->rcx);
   originalstate->rdx=getRDX();
   Print(L"originalstate->rdx=%lx\n",originalstate->rdx);
   originalstate->rsi=getRSI();
   Print(L"originalstate->rsi=%lx\n",originalstate->rsi);
   originalstate->rdi=getRDI();
   Print(L"originalstate->rdi=%lx\n",originalstate->rdi);
   originalstate->r8=getR8();
   Print(L"originalstate->r8=%lx\n",originalstate->r8);
   originalstate->r9=getR9();
   Print(L"originalstate->r9=%lx\n",originalstate->r9);
   originalstate->r10=getR10();
   Print(L"originalstate->r10=%lx\n",originalstate->r10);
   originalstate->r11=getR11();
   Print(L"originalstate->r11=%lx\n",originalstate->r11);
   originalstate->r12=getR12();
   Print(L"originalstate->r12=%lx\n",originalstate->r12);
   originalstate->r13=getR13();
   Print(L"originalstate->r13=%lx\n",originalstate->r13);
   originalstate->r14=getR14();
   Print(L"originalstate->r14=%lx\n",originalstate->r14);
   originalstate->r15=getR15();
   Print(L"originalstate->r15=%lx\n",originalstate->r15);

    originalstate->rsp-=8; //adjust rsp for the "call entervmmprologue"
    originalstate->rip=(UINT_PTR)enterVMMEpilogue; //enterVMMEpilogue is an address inside the entervmmprologue function

    Print(L"originalstate->rip=%lx\n",originalstate->rip);





    Print(L"Calling entervmm2. (Originalstate=%lx (%lx))\n",originalstate,originalstatePA);

    //call to entervmmprologue, pushes the return value on the stack


   // Input(L"Type something : ", something, 200);

    enterVMMPrologue();

    enableInterrupts();


    Print(L"\nReturned from enterVMMPrologue\n");

    Print(L"Testing:\n");

    {

      struct
      {
        unsigned int structsize;
        unsigned int level2pass;
        unsigned int command;
      } __attribute__((__packed__)) vmcallinfo;

      ZeroMem(&vmcallinfo, sizeof(vmcallinfo));

      Print(L"&vmcallinfo=%lx\n", &vmcallinfo);

      vmcallinfo.structsize=sizeof(vmcallinfo);
      vmcallinfo.level2pass=0xfedcba98;
      vmcallinfo.command=0; //VMCALL_GETVERSION

      UINT64 dbvmversion=dovmcall(&vmcallinfo, 0x76543210);
      int r;

      disableInterrupts();
      r=doSystemTest(); //check if the system behaves like it should
      enableInterrupts();

      if (r)
      {
        Print(L"Failed to pass test %d\n", r);
      }



      Print(L"still alive\ndbvmversion=%x\n", dbvmversion);
    }

    //DbgPrint("cpunr=%d\n",cpunr());


    Input(L"Type something : ", something, 200);
    Print(L"Returning\n");

}
