local web_dir = "bin/weweb"
local artifact = {
    target = { kind = "bin", name = "owe-web" },
    directory = web_dir,
}
local runtime = {
    set = "runtime",
    destination = web_dir,
}

if lito.target:match("-linux") then
    runtime.dependency = "cef"
    artifact.runtime_search = {
        { external_asset = { dependency = "cef", set = "runtime" } },
    }
    runtime.strip = {
        mode = "symbols",
        files = {
            "chrome-sandbox",
            "libEGL.so",
            "libGLESv2.so",
            "libcef.so",
            "libvk_swiftshader.so",
            "libvulkan.so.1",
        },
    }
else
    runtime.dependency = "cef-macos"
end

lito.install({
    artifacts = { artifact },
    external_assets = { runtime },
})
