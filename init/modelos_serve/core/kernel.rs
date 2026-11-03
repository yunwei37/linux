// SPDX-License-Identifier: MIT
//! C ABI: caller holds core_lock, including across synchronous GPU completion.
#![no_std]
#[path="src/objects.rs"]
mod objects;
use objects::*;
include!("image.rs");
extern "C" {
    fn mos_engine_load() -> i32;
    fn mos_engine_reserve(w:*const Work,prompt:*const u32) -> i32;
    fn mos_engine_batch(phase:u32,w:*const Work,out:*mut u32) -> i32;
    fn mos_engine_release(w:*const Work) -> i32;
    fn mos_engine_unload() -> i32;
    fn mos_panic() -> !;
}
fn checked(status:i32) -> Result<()> {if status==0 {Ok(())} else {Err(Error::Engine)}}
struct Gpu;
impl Engine for Gpu {
    fn load(&mut self,_:&Image)->Result<()> {checked(unsafe {mos_engine_load()})}
    fn reserve(&mut self,w:Work,prompt:&[u32],_:u32)->Result<()> {
        checked(unsafe {mos_engine_reserve(&w,prompt.as_ptr())})
    }
    fn run_batch(&mut self,p:Phase,w:&[Work],out:&mut [u32])->Result<()> {
        // The current AlpinDale native ABI is B1. Do not pretend a loop is a GPU batch.
        if w.len()!=1 {return Err(Error::Invalid);}
        checked(unsafe {mos_engine_batch(p as u32,w.as_ptr(),out.as_mut_ptr())})
    }
    fn release(&mut self,w:Work)->Result<()> {checked(unsafe {mos_engine_release(&w)})}
    fn unload(&mut self)->Result<()> {checked(unsafe {mos_engine_unload()})}
}
static mut SERVICE:Option<Instance<Gpu>>=None;
unsafe fn service()-> &'static mut Instance<Gpu> {
    // SAFETY: this module is private; all C entrypoints require the same mutex.
    (*core::ptr::addr_of_mut!(SERVICE)).as_mut().unwrap()
}
fn errno<T>(r:Result<T>) -> i32 {
    match r {Ok(_)=>0,Err(Error::Capacity)=>-28,Err(Error::Stale)=>-116,
        Err(Error::Invalid)=>-22,Err(Error::State)=>-16,Err(_)=>-5}
}
#[no_mangle]
pub unsafe extern "C" fn mos_init(round_robin:u32)->i32 {
    if (*core::ptr::addr_of!(SERVICE)).is_some() {return -16;}
    let mut image=Image::candidate(IMAGE_ID,IMAGE_BOUNDS);
    if image.validate(IMAGE_ID).is_err() {return -22;}
    match Instance::load(image,Gpu,if round_robin==0 {Policy::Deadline} else {Policy::RoundRobin}) {
        Ok(s)=>{core::ptr::addr_of_mut!(SERVICE).write(Some(s));0},Err(_)=>-5
    }
}
#[no_mangle]
pub unsafe extern "C" fn mos_admit(session:u64,id:u32,prompt:*const u32,n:u32,budget:u32,now:u64,h:*mut Handle)->i32 {
    if n==0 || n>IMAGE_BOUNDS.max_prompt {return -22;}
    match service().admit(Admission {session,request:id,output:budget,
        first_deadline:now.saturating_add(1_000_000_000),token_interval:50_000_000},
        core::slice::from_raw_parts(prompt,n as usize)) {
        Ok(handle)=>{h.write(handle);0},Err(e)=>errno::<()>(Err(e))
    }
}
#[no_mangle]
pub unsafe extern "C" fn mos_advance(out:*mut Event)->i32 {
    let mut events=[None;SLOTS];
    match service().advance(&mut events) {
        Ok(0)=>0,Ok(1)=>{out.write(events[0].unwrap());1},Ok(_)=>-22,Err(e)=>errno::<()>(Err(e))
    }
}
#[no_mangle]
pub unsafe extern "C" fn mos_cancel(session:u64,h:Handle)->i32 {errno(service().cancel(session,h))}
#[no_mangle]
pub unsafe extern "C" fn mos_ack(session:u64,h:Handle,now:u64)->i32 {errno(service().acknowledge(session,h,now))}
#[no_mangle]
pub unsafe extern "C" fn mos_stats(out:*mut Accounting) {out.write(service().accounting());}
#[no_mangle]
pub unsafe extern "C" fn mos_stop()->i32 {
    let s=service(); if s.accounting().live!=0 {return -16;}
    if let Err(e)=s.quiesce() {return errno::<()>(Err(e));} errno(s.retire())
}
#[panic_handler]
fn panic(_:&core::panic::PanicInfo<'_>)->! {unsafe {mos_panic()}}
