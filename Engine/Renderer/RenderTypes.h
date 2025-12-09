#pragma once

#include <vector>
#include <glm/glm.hpp>
#include "Utilities/UUID.h"

struct DrawCommand
{
	TerraEngine::UUID meshId;
	TerraEngine::UUID materialId;
	glm::mat4 modelMatrix;
};

struct FrameRenderData
{
	glm::mat4 viewMatrix;
	glm::mat4 projectionMatrix;
	std::vector<DrawCommand> commands;
};
