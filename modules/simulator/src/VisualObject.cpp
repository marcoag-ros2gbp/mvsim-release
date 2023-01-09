/*+-------------------------------------------------------------------------+
  |                       MultiVehicle simulator (libmvsim)                 |
  |                                                                         |
  | Copyright (C) 2014-2023  Jose Luis Blanco Claraco                       |
  | Copyright (C) 2017  Borys Tymchenko (Odessa Polytechnic University)     |
  | Distributed under 3-clause BSD License                                  |
  |   See COPYING                                                           |
  +-------------------------------------------------------------------------+ */

#include <mrpt/opengl/CBox.h>
#include <mrpt/opengl/COpenGLScene.h>
#include <mrpt/opengl/CSetOfObjects.h>
#include <mrpt/version.h>
#include <mvsim/Block.h>
#include <mvsim/Simulable.h>
#include <mvsim/VisualObject.h>
#include <mvsim/World.h>

#include <atomic>
#include <rapidxml.hpp>

#include "ModelsCache.h"
#include "xml_utils.h"

using namespace mvsim;

static std::atomic_int32_t g_uniqueCustomVisualId = 0;

VisualObject::~VisualObject() = default;

void VisualObject::guiUpdate(
	const mrpt::optional_ref<mrpt::opengl::COpenGLScene>& viz,
	const mrpt::optional_ref<mrpt::opengl::COpenGLScene>& physical)
{
	using namespace std::string_literals;

	const auto* meSim = dynamic_cast<Simulable*>(this);
	ASSERT_(meSim);

	// If "viz" does not have a value, it's because we are already inside a
	// setPose() change event, so my caller already holds the mutex and we don't
	// need/can't acquire it again:
	const auto objectPose =
		viz.has_value() ? meSim->getPose() : meSim->getPoseNoLock();

	if (glCustomVisual_ && viz.has_value() && physical.has_value())
	{
		// Assign a unique ID on first call:
		if (glCustomVisualId_ < 0)
		{
			// Assign a unique name, so we can localize the object in the scene
			// if needed.
			glCustomVisualId_ = g_uniqueCustomVisualId++;
			const auto name = "_autoViz"s + std::to_string(glCustomVisualId_);
			glCustomVisual_->setName(name);

			// Add to the 3D scene:
			if (insertCustomVizIntoViz_) viz->get().insert(glCustomVisual_);

			if (insertCustomVizIntoPhysical_)
				physical->get().insert(glCustomVisual_);
		}

		// Update pose:
		glCustomVisual_->setPose(objectPose);
	}

	if (glBoundingBox_ && viz.has_value())
	{
		if (glBoundingBox_->empty())
		{
			auto glBox = mrpt::opengl::CBox::Create();
			glBox->setWireframe(true);
			glBox->setBoxCorners(viz_bbmin_, viz_bbmax_);
			glBoundingBox_->insert(glBox);
			glBoundingBox_->setVisibility(false);
			viz->get().insert(glBoundingBox_);
		}
		glBoundingBox_->setPose(objectPose);
	}

	const bool childrenOnly = !!glCustomVisual_;

	internalGuiUpdate(viz, physical, childrenOnly);
}

void VisualObject::FreeOpenGLResources() { ModelsCache::Instance().clear(); }

bool VisualObject::parseVisual(const rapidxml::xml_node<char>* visual_node)
{
	MRPT_TRY_START

	glBoundingBox_ = mrpt::opengl::CSetOfObjects::Create();
	glBoundingBox_->setName("bbox");

	if (visual_node == nullptr) return false;

	std::string modelURI;
	double modelScale = 1.0;
	mrpt::math::TPose3D modelPose;
	bool initialShowBoundingBox = false;

	ModelsCache::Options opts;

	TParameterDefinitions params;
	params["model_uri"] = TParamEntry("%s", &modelURI);
	params["model_scale"] = TParamEntry("%lf", &modelScale);
	params["model_offset_x"] = TParamEntry("%lf", &modelPose.x);
	params["model_offset_y"] = TParamEntry("%lf", &modelPose.y);
	params["model_offset_z"] = TParamEntry("%lf", &modelPose.z);
	params["model_yaw"] = TParamEntry("%lf_deg", &modelPose.yaw);
	params["model_pitch"] = TParamEntry("%lf_deg", &modelPose.pitch);
	params["model_roll"] = TParamEntry("%lf_deg", &modelPose.roll);
	params["show_bounding_box"] = TParamEntry("%bool", &initialShowBoundingBox);
	params["model_cull_faces"] = TParamEntry("%s", &opts.modelCull);
	params["model_color"] = TParamEntry("%color", &opts.modelColor);

	// Parse XML params:
	parse_xmlnode_children_as_param(*visual_node, params);

	if (modelURI.empty()) return false;

	const std::string localFileName = world_->xmlPathToActualPath(modelURI);

	auto glGroup = mrpt::opengl::CSetOfObjects::Create();

	auto& gModelsCache = ModelsCache::Instance();

	auto glModel = gModelsCache.get(localFileName, opts);

	// Make sure the points and vertices buffers are up to date, so we can
	// access them:
	glModel->onUpdateBuffers_all();

	mrpt::math::TBoundingBox bb = mrpt::math::TBoundingBox::PlusMinusInfinity();
	// Slice bbox in z up to a given relevant height:
	if (const Block* block = dynamic_cast<const Block*>(this); block)
	{
		const auto zMin = block->block_z_min();
		const auto zMax = block->block_z_max();

		size_t numTotalPts = 0, numPassedPts = 0;

		auto lambdaUpdatePt = [&](const mrpt::math::TPoint3Df& orgPt) {
			numTotalPts++;
			auto pt = modelPose.composePoint(orgPt * modelScale);
			if (pt.z < zMin || pt.z > zMax) return;	 // skip
			bb.updateWithPoint(pt);
			numPassedPts++;
		};

		{
			auto lck =
				mrpt::lockHelper(glModel->shaderTrianglesBufferMutex().data);
			const auto& tris = glModel->shaderTrianglesBuffer();
			for (const auto& tri : tris)
				for (const auto& v : tri.vertices) lambdaUpdatePt(v.xyzrgba.pt);
		}
		{
			auto lck =
				mrpt::lockHelper(glModel->shaderPointsBuffersMutex().data);
			const auto& pts = glModel->shaderPointsVertexPointBuffer();
			for (const auto& pt : pts) lambdaUpdatePt(pt);
		}
		{
			auto lck =
				mrpt::lockHelper(glModel->shaderWireframeBuffersMutex().data);
			const auto& pts = glModel->shaderWireframeVertexPointBuffer();
			for (const auto& pt : pts) lambdaUpdatePt(pt);
		}

#if MRPT_VERSION >= 0x260
		const auto& txtrdObjs =
			glModel->texturedObjects();	 // [new mrpt v2.6.0]
		for (const auto& obj : txtrdObjs)
		{
			if (!obj) continue;

			auto lck = mrpt::lockHelper(
				obj->shaderTexturedTrianglesBufferMutex().data);
			const auto& tris = obj->shaderTexturedTrianglesBuffer();
			for (const auto& tri : tris)
				for (const auto& v : tri.vertices) lambdaUpdatePt(v.xyzrgba.pt);
		}
#endif
#if 0
		std::cout << "bbox for [" << modelURI << "] numTotalPts=" << numTotalPts
				  << " numPassedPts=" << numPassedPts << " zMin = " << zMin
				  << " zMax=" << zMax << " bb=" << bb.asString() << "\n";
#endif
	}

	if (bb.min == mrpt::math::TBoundingBox::PlusMinusInfinity().min ||
		bb.max == mrpt::math::TBoundingBox::PlusMinusInfinity().max)
	{
		// default: the whole model bbox:
		bb = glModel->getBoundingBox();

		// Apply transformation to bounding box too:
		bb.min = modelPose.composePoint(bb.min * modelScale);
		bb.max = modelPose.composePoint(bb.max * modelScale);
	}

	glGroup->insert(glModel);

	glGroup->setScale(modelScale);
	glGroup->setPose(modelPose);
	glGroup->setName("group");

	glCustomVisual_ = mrpt::opengl::CSetOfObjects::Create();
	glCustomVisual_->setName("glCustomVisual");
	glCustomVisual_->insert(glGroup);
	glBoundingBox_->setVisibility(initialShowBoundingBox);

	// Auto bounds from visual model bounding-box:

	// Apply transformation to bounding box too:
	viz_bbmin_ = bb.min;
	viz_bbmax_ = bb.max;

	return true;
	MRPT_TRY_END
}

void VisualObject::showBoundingBox(bool show)
{
	if (!glBoundingBox_) return;
	glBoundingBox_->setVisibility(show);
}

void VisualObject::customVisualVisible(const bool visible)
{
	if (!glCustomVisual_) return;

	glCustomVisual_->setVisibility(visible);
}

bool VisualObject::customVisualVisible() const
{
	return glCustomVisual_ && glCustomVisual_->isVisible();
}
