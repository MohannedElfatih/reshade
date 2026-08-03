struct FallbackMeshVertex
{
	float4 position : SV_Position;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(out vertices FallbackMeshVertex vertices[1], out indices uint3 triangles[1])
{
	SetMeshOutputCounts(0, 0);
	vertices[0].position = 0;
	triangles[0] = 0;
}
