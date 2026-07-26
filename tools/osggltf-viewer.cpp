// vimrun! ./tools/osggltf-viewer model.gltf --ktx2 papermill.ktx2 --hdr papermill.hdr
//
// osgGLTF-owned viewer for its optional osgx-powered PBR/IBL renderer.
//
// lutCamera (returned by createPBRIBLScene) MUST be added to the scene graph or the
// BRDF LUT never bakes; it's ABSOLUTE_RF, so it doesn't matter where.

#include <osgGLTF/PBR.hpp>

#include <osgx/Callbacks.hpp>
#include <osgx/Core.hpp>
#include <osgx/Warnings.hpp>

OSGX_DISABLE_WARNINGS

#define TINYGLTF_NOEXCEPTION

#include "tiny_gltf.h"

#include <osg/ArgumentParser>
#include <osg/Camera>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osgDB/WriteFile>
#include <osg/DisplaySettings>
#include <osg/Group>
#include <osg/Image>
#include <osg/Notify>
#include <osg/Vec3>
#include <osg/Vec4>
#include <osg/observer_ptr>
#include <osgGA/GUIActionAdapter>
#include <osgGA/GUIEventAdapter>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <atomic>
#include <iostream>
#include <map>
#include <numbers>
#include <string>
#include <utility>

namespace {

class FramebufferPNG: public osg::Camera::DrawCallback {
public:
	FramebufferPNG(osg::Camera* camera, std::string filename):
		_camera(camera),
		_filename(std::move(filename)) {}

	void operator()(osg::RenderInfo&) const override {
		if(_started.exchange(true, std::memory_order_acq_rel)) return;

		const auto* viewport = _camera->getViewport();

		if(!viewport || !viewport->valid()) {
			OSG_WARN << "Capture camera has no viewport" << std::endl;
			_done.store(true, std::memory_order_release);

			return;
		}

		auto image = osgx::make_ref<osg::Image>();

		image->readPixels(
			static_cast<int>(viewport->x()),
			static_cast<int>(viewport->y()),
			static_cast<int>(viewport->width()),
			static_cast<int>(viewport->height()),
			GL_RGB,
			GL_UNSIGNED_BYTE
		);
		_success.store(osgDB::writeImageFile(*image, _filename), std::memory_order_relaxed);
		_done.store(true, std::memory_order_release);

		if(_success.load(std::memory_order_relaxed)) {
			OSG_NOTICE << "Wrote framebuffer screenshot: " << _filename << std::endl;
		}

		else OSG_WARN << "Failed to write framebuffer screenshot: " << _filename << std::endl;
	}

	bool done() const {
		return _done.load(std::memory_order_acquire);
	}

	bool success() const {
		return _success.load(std::memory_order_relaxed);
	}

private:
	osg::observer_ptr<osg::Camera> _camera;
	std::string _filename;
	mutable std::atomic<bool> _started{false};
	mutable std::atomic<bool> _done{false};
	mutable std::atomic<bool> _success{false};
};

bool applyKhronosCamera(osg::Camera* camera, const std::string& filename) {
	tinygltf::TinyGLTF loader;
	tinygltf::Model document;
	std::string error, warning;

	if(!loader.LoadASCIIFromFile(&document, &error, &warning, filename)) {
		std::cerr << "Failed to read camera export '" << filename << "': " << error << std::endl;

		return false;
	}

	if(!warning.empty()) std::cerr << "Camera export warning: " << warning << std::endl;

	if(document.cameras.empty()) {
		std::cerr << "Camera export contains no cameras: " << filename << std::endl;

		return false;
	}

	const tinygltf::Node* node = nullptr;

	for(const auto& candidate : document.nodes) {
		if(candidate.camera >= 0) {
			node = &candidate;

			break;
		}
	}

	if(!node || node->matrix.size() != 16) {
		std::cerr << "Camera export needs a camera node with a 4x4 matrix: " << filename << std::endl;

		return false;
	}

	const std::size_t cameraIndex = static_cast<std::size_t>(node->camera);

	if(cameraIndex >= document.cameras.size()) {
		std::cerr << "Camera export references an invalid camera: " << filename << std::endl;

		return false;
	}

	const auto& perspective = document.cameras[cameraIndex].perspective;

	if(perspective.yfov <= 0.0 || perspective.aspectRatio <= 0.0 || perspective.znear <= 0.0 || perspective.zfar <= perspective.znear) {
		std::cerr << "Camera export has an unsupported perspective projection: " << filename << std::endl;

		return false;
	}

	const auto& matrix = node->matrix;
	const auto zUp = [](double x, double y, double z) {
		return osg::Vec3d(x, -z, y);
	};
	const osg::Vec3d eye = zUp(matrix[12], matrix[13], matrix[14]);
	const osg::Vec3d forward = zUp(-matrix[8], -matrix[9], -matrix[10]);
	const osg::Vec3d up = zUp(matrix[4], matrix[5], matrix[6]);

	camera->setViewMatrixAsLookAt(eye, eye + forward, up);
	camera->setProjectionMatrixAsPerspective(
		perspective.yfov * 180.0 / std::numbers::pi,
		perspective.aspectRatio,
		perspective.znear,
		perspective.zfar
	);

	return true;
}

}

int main(int argc, char** argv) {
	// Khronos requests an antialiased WebGL2 context. WebGL leaves the exact sample count to the
	// browser; the desktop path used for these parity captures ordinarily resolves to 4x MSAA.
	osg::DisplaySettings::instance()->setNumMultiSamples(4);

	osg::ArgumentParser args(&argc, argv);
	osgViewer::Viewer viewer(args);

	args.getApplicationUsage()->setCommandLineUsage(
		std::string(args.getApplicationName()) +
		" <model.gltf> --ktx2 <path> --hdr <path> [--camera <camera.gltf>] [--capture <path.png>] [--debug [mode]]"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--ktx2 <path>",
		"Pre-filtered environment cubemap"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--hdr <path>",
		"Source HDR environment (for Lambertian diffuse irradiance)"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--capture <path.png>",
		"Write the first complete framebuffer to a PNG and exit"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--camera <path.gltf>",
		"Apply a Khronos Sample Viewer camera export"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--debug [mode]",
		"combined, diffuse, specular, base-color, roughness, metallic, normal-texture, normal-texture-raw, geometry-normal, shading-normal, geometry-tangent, bitangent, linear-diffuse, linear-specular, or linear-combined"
	);

	std::string ktx2Path, hdrPath, cameraPath, capturePath, debugName = "combined";
	const std::map<std::string, int> debugModes = {
		{"combined", 0}, {"diffuse", 1}, {"specular", 2},
		{"base-color", 3}, {"roughness", 4}, {"metallic", 5},
		{"normal-texture", 6}, {"normal-texture-raw", 7}, {"geometry-normal", 8},
		{"shading-normal", 9}, {"geometry-tangent", 10}, {"bitangent", 11},
		{"linear-diffuse", 12}, {"linear-specular", 13}, {"linear-combined", 14}
	};

	const bool haveKtx2 = args.read("--ktx2", ktx2Path);
	const bool haveHdr = args.read("--hdr", hdrPath);
	const bool haveCamera = args.read("--camera", cameraPath);
	const bool captureRequested = args.read("--capture", capturePath);
	const int debugPos = args.find("--debug");
	const bool diagnostics = debugPos >= 0;

	if(diagnostics) {
		const bool hasMode = debugPos + 1 < args.argc() && debugModes.contains(args[debugPos + 1]);

		if(hasMode) args.read(debugPos, "--debug", debugName);
		else args.read(debugPos, "--debug");
	}

	if(args.argc() < 2 || !haveKtx2 || !haveHdr) {
		args.getApplicationUsage()->write(std::cerr);

		return 1;
	}

	// ReaderWriterGLTF registers this same alias in its own constructor, but that
	// constructor only runs *after* the registry has already resolved which plugin
	// library to dlopen for a given extension -- too late for a cold ".glb" load.
	// Registering it here first breaks the chicken-and-egg.
	osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");

	osg::ref_ptr<osg::Node> model = osgDB::readRefNodeFile(args[1]);

	if(!model) {
		std::cerr << "Failed to load: " << args[1] << std::endl;

		return 1;
	}

	auto pis = osgGLTF::pbr::createPBRIBLScene(
		model,
		ktx2Path,
		hdrPath,
		1.0f,
		1024,
		diagnostics
	);

	if(!pis.valid()) {
		std::cerr << "createPBRIBLScene failed to load " << ktx2Path << " / " << hdrPath << std::endl;

		return 1;
	}

	const auto debug = debugModes.find(debugName);

	if(diagnostics && debug == debugModes.end()) {
		std::cerr << "Unknown --debug mode: " << debugName << std::endl;

		return 1;
	}

	if(diagnostics) pis.debugMode->set(debug->second);

	auto root = osgx::make_ref<osg::Group>();

	root->addChild(pis.lutCamera);
	root->addChild(model);

	viewer.setSceneData(root);
	viewer.addEventHandler(new osgViewer::StatsHandler());
	viewer.getCamera()->setClearColor(osg::Vec4f(
		48.0f / 255.0f,
		53.0f / 255.0f,
		66.0f / 255.0f,
		1.0f
	)); // #303542

	if(haveCamera && !applyKhronosCamera(viewer.getCamera(), cameraPath)) return 1;

	// Diagnostics, ported from pyosg-khronos-viewer.py: 1/2/3 pick debugMode; N/R toggle the
	// normal/roughness maps.
	if(diagnostics) std::cout <<
		"Diagnostics: 1=combined 2=diffuse 3=specular N=toggle normal map "
		"R=toggle roughness map" << std::endl
	;

	if(diagnostics) viewer.addEventHandler(new osgx::LambdaKeyHandler(
		{'1', '2', '3', 'n', 'N', 'r', 'R'},
		[pis](const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter&, int key) {
			switch(key) {
				case '1': {
					pis.debugMode->set(0);

					std::cout << "[diagnostic] combined" << std::endl;

					break;
				}

				case '2': {
					pis.debugMode->set(1);

					std::cout << "[diagnostic] diffuse only" << std::endl;

					break;
				}

				case '3': {
					pis.debugMode->set(2);

					std::cout << "[diagnostic] specular only" << std::endl;

					break;
				}

				case 'n': case 'N': {
					int v = 0;

					pis.disableNormalMap->get(v);
					pis.disableNormalMap->set(1 - v);

					std::cout << "[diagnostic] normal map " << (v ? "on" : "off") << std::endl;

					break;
				}

				case 'r': case 'R': {
					int v = 0;

					pis.disableRoughnessMap->get(v);
					pis.disableRoughnessMap->set(1 - v);

					std::cout << "[diagnostic] roughness map " << (v ? "on" : "off") << std::endl;

					break;
				}

				default: return false;
			}

			return true;
		}
	));

	osg::ref_ptr<FramebufferPNG> capture;

	if(captureRequested) {
		capture = new FramebufferPNG(viewer.getCamera(), capturePath);
		viewer.getCamera()->setFinalDrawCallback(capture);
	}

	while(!viewer.done()) {
		viewer.frame();

		if(capture && capture->done()) break;
	}

	return capture && !capture->success() ? 1 : 0;
}
