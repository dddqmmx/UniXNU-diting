// SPDX-License-Identifier: MIT
// Exercise the selected frontend's real container, bitcode, verifier and AIR ABI.
use metal2vulkan_air::{budget::{Budget, Meter}, container::{self, Library}, decode};
use std::{env, fs};
fn main() {
    let result = (|| -> Result<(), Box<dyn std::error::Error>> {
        let path = env::args().nth(1).ok_or("owned metallib path required")?;
        let raw = fs::read(path)?;
        if raw.len() > 1024 * 1024 { return Err("owned library bound exceeded".into()); }
        let Library::Metallib(library) = container::parse(&raw).map_err(|e| format!("container: {e:?}"))? else {
            return Err("metallib required".into());
        };
        if library.functions.len() != 3 { return Err("three owned functions required".into()); }
        for (entry, name) in library.functions.iter().zip(["diting_test_vertex", "diting_test_fragment", "diting_test_compute"]) {
            if entry.name != name { return Err("owned function list mismatch".into()); }
            let mut meter = Meter::new(&Budget::standard());
            let mut decoded = decode(entry.bitcode, &mut meter).map_err(|e| format!("{} decode: {e:?}", entry.name))?;
            decoded.abi.check_container(&decoded.module, entry, &mut meter)
                .map_err(|e| format!("{} ABI: {e:?}", entry.name))?;
            println!("PASS {} container, IR and AIR ABI", entry.name);
        }
        Ok(())
    })();
    if let Err(error) = result { eprintln!("FAIL {error}"); std::process::exit(1); }
}
