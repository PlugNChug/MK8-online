fn main() {
    println!("cargo:rerun-if-env-changed=MK8_RELAY_URL");
    println!("cargo:rerun-if-env-changed=MK8_UPDATE_REPO");
    println!("cargo:rerun-if-env-changed=MK8_LAUNCHER_PAYLOAD");
    println!("cargo:rerun-if-env-changed=MK8_LAUNCHER_PAYLOAD_ID");
    println!("cargo:rustc-check-cfg=cfg(mk8_portable)");
    if let Ok(path) = std::env::var("MK8_LAUNCHER_PAYLOAD") {
        println!("cargo:rerun-if-changed={path}");
        println!("cargo:rustc-cfg=mk8_portable");
    }
    tauri_build::build();
}
