// SPDX-License-Identifier: LGPL-3.0-or-later
// Threadgroup buffer arguments (air.address_space 3) are lowered to a raw word view of Workgroup memory, but when the
// argument's metadata carries air.struct_type_info (Dawn/Tint: `threadgroup tint_struct_5* v` = { array<half4, 128> }),
// the native emitter types the Workgroup variable as that struct and then loads words from the struct pointer itself
// ("owned Load violates its pointer-pointee and value-type contract ... TypeStruct ... result type TypeInt 32").
// Without the struct description the same module translates and validates (Chrome's
// on-device model service died on this). The layout of threadgroup memory is private to
// the kernel, so the description adds nothing there.

/// The module with air.struct_type_info removed from every threadgroup buffer argument, or None if it has none.
pub fn strip_threadgroup_struct_info(ll: &str) -> Option<String> {
    let mut changed = false;
    let mut out = String::with_capacity(ll.len());
    for line in ll.split_inclusive('\n') {
        if line.starts_with('!') && line.contains("!\"air.address_space\", i32 3") {
            if let Some(at) = line.find("!\"air.struct_type_info\", !") {
                let rest = &line[at + "!\"air.struct_type_info\", !".len()..];
                let digits = rest.bytes().take_while(|b| b.is_ascii_digit()).count();
                if digits > 0 && rest[digits..].starts_with(", ") {
                    out.push_str(&line[..at]);
                    out.push_str(&rest[digits + 2..]);
                    changed = true;
                    continue;
                }
            }
        }
        out.push_str(line);
    }
    changed.then_some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strips_only_threadgroup_struct_info() {
        let ll = "!9 = !{i32 1, !\"air.buffer\", !\"air.address_space\", i32 1, !\"air.struct_type_info\", !4, !\"air.arg_name\", !\"out\"}\n\
                  !10 = !{i32 2, !\"air.buffer\", !\"air.address_space\", i32 3, !\"air.struct_type_info\", !11, !\"air.arg_type_size\", i32 1024}\n";
        let s = strip_threadgroup_struct_info(ll).expect("changed");
        assert!(s.contains("i32 1, !\"air.struct_type_info\", !4"), "{s}");
        assert!(s.contains("i32 3, !\"air.arg_type_size\", i32 1024"), "{s}");
        assert_eq!(strip_threadgroup_struct_info("define void @k() {\n}\n"), None);
    }

    #[test]
    fn dawn_threadgroup_struct_argument_translates() {
        let ll = include_str!("../tests/data/tg-half4-min.ll");
        let st = metal2vulkan::passes::Stage::Kernel;
        assert!(crate::translate_one(ll, st).is_ok(), "{:?}", crate::translate_one(ll, st).err());
    }
}
