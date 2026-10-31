// SPDX-License-Identifier: GPL-2.0-only
//! Kernel-only Rust entry; Linux worker primitives are provided by the C bridge.
use kernel::prelude::*;

module! {
    type: ModelosRust,
    name: "modelos_rust",
    authors: ["ModelOS contributors"],
    description: "ModelOS built-in Rust entry",
    license: "GPL",
}

struct ModelosRust;

impl kernel::Module for ModelosRust {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("ModelOS Rust module registered\n");
        Ok(Self)
    }
}

unsafe extern "C" {
    fn modelos_enter() -> !;
}

#[unsafe(no_mangle)]
pub extern "C" fn modelos_rust_enter() -> ! {
    pr_info!("MODELOS_RUST_ENTRY after_initcalls=1\n");
    // SAFETY: kernel_init calls this after kernel initialization, in process
    // context with IRQs enabled. The C bridge never returns or enters user mode.
    unsafe { modelos_enter() }
}
