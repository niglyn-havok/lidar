#include "Visuals/DublinAircraftVisual.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDublinAircraftAirframeGeometryTest, "DublinFlight.Visuals.AirframeGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDublinAircraftAirframeGeometryTest::RunTest(const FString& Parameters)
{
	using namespace DublinAircraftVisual;
	const DublinAircraftVisual::FGeometry Geometry = BuildGeometry();
	TestEqual(TEXT("Five bounded opaque airframe sections"), Geometry.Airframe.Num(), FinishCount);
	TestEqual(TEXT("Propeller uses the shared graphite finish"),
		static_cast<uint8>(Geometry.Propeller.Finish), static_cast<uint8>(EFinish::Graphite));
	TestEqual(TEXT("Opaque canopy roughness"), GetFinish(EFinish::Canopy).Roughness, 0.12f);
	FBox Bounds(ForceInit);
	int32 TriangleCount = 0;
	int32 VertexCount = 0;
	bool bFinite = true;
	bool bIndicesValid = true;
	bool bClockwise = true;
	bool bUnitNormals = true;
	bool bTangentsValid = true;
	bool bUVsConsistent = true;
	bool bInsideCollisionEnvelope = true;
	bool bRotorInsideEnvelope = true;
	bool bUpperWingFacesUp = false;
	bool bLeftUpperWingFacesUp = false;
	bool bRightUpperWingFacesUp = false;
	bool bCanopyFacesUp = false;
	bool bUpperCanopyNormalsOutward = true;
	double WingMinZ = TNumericLimits<double>::Max();
	double WingMaxZ = -TNumericLimits<double>::Max();
	const auto Inspect = [&](const FSection& Section, bool bPropeller)
	{
		const FString Label = bPropeller ? TEXT("Propeller") : FString::Printf(TEXT("Finish %d"), static_cast<int32>(Section.Finish));
		TestTrue(*(Label + TEXT(" has geometry")), !Section.Vertices.IsEmpty() && !Section.Triangles.IsEmpty());
		const bool bBuffersMatch = Section.Normals.Num() == Section.Vertices.Num()
			&& Section.UV.Num() == Section.Vertices.Num() && Section.Tangents.Num() == Section.Vertices.Num();
		TestTrue(*(Label + TEXT(" vertex buffers match")), bBuffersMatch);
		TestEqual(*(Label + TEXT(" complete triangles")), Section.Triangles.Num() % 3, 0);
		if (!bBuffersMatch) { return; }
		VertexCount += Section.Vertices.Num();
		TriangleCount += Section.Triangles.Num() / 3;
		bool bReportedNormalFailure = false;
		bool bReportedWindingFailure = false;
		bool bReportedUVFailure = false;
		const auto ReportFirst = [&](bool bValid, bool& bReported, const FString& Detail)
		{
			if (!bValid && !bReported)
			{
				AddError(Label + TEXT(": ") + Detail);
				bReported = true;
			}
		};
		for (int32 Index = 0; Index < Section.Vertices.Num(); ++Index)
		{
			const FVector& Local = Section.Vertices[Index];
			const FVector Position = Local + (bPropeller ? FVector(PropellerOffsetCm, 0.0, 0.0) : FVector::ZeroVector);
			const FVector& Normal = Section.Normals[Index];
			const FVector& Tangent = Section.Tangents[Index].TangentX;
			const FVector2D& UV = Section.UV[Index];
			bFinite &= !Position.ContainsNaN() && !Normal.ContainsNaN() && !Tangent.ContainsNaN()
				&& FMath::IsFinite(UV.X) && FMath::IsFinite(UV.Y);
			const bool bUnitNormal = !Normal.ContainsNaN() && FMath::IsNearlyEqual(Normal.SizeSquared(), 1.0, 0.0001);
			bUnitNormals &= bUnitNormal;
			ReportFirst(bUnitNormal, bReportedNormalFailure,
				FString::Printf(TEXT("vertex %d normal squared length %.12g"), Index, Normal.SizeSquared()));
			bTangentsValid &= FMath::IsNearlyEqual(Tangent.SizeSquared(), 1.0, 0.0001)
				&& FMath::Abs(FVector::DotProduct(Normal, Tangent)) < 0.0001;
			bInsideCollisionEnvelope &= Position.SizeSquared() <= FMath::Square(470.0);
			Bounds += Position;
			if (bPropeller)
			{
				// Rotation around X cannot change the radial extent or the sphere fit.
				bRotorInsideEnvelope &= FMath::Square(PropellerOffsetCm + Local.X)
					+ Local.Y * Local.Y + Local.Z * Local.Z < FMath::Square(470.0);
			}
			else if (FMath::Abs(Position.Y) > 200.0 && FMath::Abs(Position.Y) < 260.0)
			{
				WingMinZ = FMath::Min(WingMinZ, Position.Z);
				WingMaxZ = FMath::Max(WingMaxZ, Position.Z);
				bUpperWingFacesUp |= Position.Z > -3.0 && Normal.Z > 0.7;
				bLeftUpperWingFacesUp |= Position.Y < 0.0 && Position.Z > -3.0 && Normal.Z > 0.7;
				bRightUpperWingFacesUp |= Position.Y > 0.0 && Position.Z > -3.0 && Normal.Z > 0.7;
			}
			if (!bPropeller && Section.Finish == EFinish::Canopy && Position.Z > 90.0)
			{
				bCanopyFacesUp |= Normal.Z > 0.7;
				bUpperCanopyNormalsOutward &= Normal.Z > 0.0;
			}
		}
		for (int32 Offset = 0; Offset + 2 < Section.Triangles.Num(); Offset += 3)
		{
			const int32 A = Section.Triangles[Offset], B = Section.Triangles[Offset + 1], C = Section.Triangles[Offset + 2];
			if (!Section.Vertices.IsValidIndex(A) || !Section.Vertices.IsValidIndex(B) || !Section.Vertices.IsValidIndex(C))
			{
				bIndicesValid = false;
				continue;
			}
			const FVector Clockwise = FVector::CrossProduct(Section.Vertices[C] - Section.Vertices[A],
				Section.Vertices[B] - Section.Vertices[A]);
			const double NormalDot = FVector::DotProduct(Clockwise,
				Section.Normals[A] + Section.Normals[B] + Section.Normals[C]);
			const bool bTriangleClockwise = Clockwise.SizeSquared() > 1.e-10 && NormalDot > 0.0;
			bClockwise &= bTriangleClockwise;
			ReportFirst(bTriangleClockwise, bReportedWindingFailure,
				FString::Printf(TEXT("triangle %d (%d,%d,%d) clockwise squared area %.12g, normal dot %.12g"),
					Offset / 3, A, B, C, Clockwise.SizeSquared(), NormalDot));
			const FVector2D D1 = Section.UV[B] - Section.UV[A], D2 = Section.UV[C] - Section.UV[A];
			const double Determinant = D1.X * D2.Y - D1.Y * D2.X;
			if (FMath::Abs(Determinant) < 1.e-12)
			{
				bUVsConsistent = false;
				ReportFirst(false, bReportedUVFailure,
					FString::Printf(TEXT("triangle %d UV determinant %.12g"), Offset / 3, Determinant));
				continue;
			}
			const FVector UDirection = ((Section.Vertices[B] - Section.Vertices[A]) * D2.Y
				- (Section.Vertices[C] - Section.Vertices[A]) * D1.Y) / Determinant;
			const FVector VDirection = ((Section.Vertices[C] - Section.Vertices[A]) * D1.X
				- (Section.Vertices[B] - Section.Vertices[A]) * D2.X) / Determinant;
			for (int32 Corner : {A, B, C})
			{
				const FProcMeshTangent& Tangent = Section.Tangents[Corner];
				const FVector Bitangent = FVector::CrossProduct(Section.Normals[Corner], Tangent.TangentX)
					* (Tangent.bFlipTangentY ? -1.0 : 1.0);
				const double UDot = FVector::DotProduct(Tangent.TangentX, UDirection);
				const double VDot = FVector::DotProduct(Bitangent, VDirection);
				const bool bCornerUVConsistent = UDot > 0.0 && VDot > 0.0;
				bUVsConsistent &= bCornerUVConsistent;
				ReportFirst(bCornerUVConsistent, bReportedUVFailure,
					FString::Printf(TEXT("triangle %d vertex %d UV tangent dots (%.12g, %.12g), flip %d"),
						Offset / 3, Corner, UDot, VDot, Tangent.bFlipTangentY ? 1 : 0));
			}
		}
	};
	for (int32 Index = 0; Index < Geometry.Airframe.Num(); ++Index)
	{
		TestEqual(TEXT("Stable section/material order"), static_cast<int32>(Geometry.Airframe[Index].Finish), Index);
		Inspect(Geometry.Airframe[Index], false);
	}
	Inspect(Geometry.Propeller, true);
	TestTrue(TEXT("All positions, normals, tangents and UVs finite"), bFinite);
	TestTrue(TEXT("All triangle indices in range"), bIndicesValid);
	TestTrue(TEXT("Nondegenerate triangles match Unreal clockwise normals"), bClockwise);
	TestTrue(TEXT("Smooth normals are normalized"), bUnitNormals);
	TestTrue(TEXT("Tangents normalized and orthogonal to normals"), bTangentsValid);
	TestTrue(TEXT("UV directions agree with tangent handedness on both wings"), bUVsConsistent);
	TestTrue(TEXT("All airframe vertices fit unchanged 470 cm collision sphere"), bInsideCollisionEnvelope);
	TestTrue(TEXT("Rotating propeller fits collision sphere at every angle"), bRotorInsideEnvelope);
	TestTrue(TEXT("Combined triangle budget"), TriangleCount >= MinimumTriangles && TriangleCount <= MaximumTriangles);
	TestTrue(TEXT("Bounded vertex buffers"), VertexCount < MaximumTriangles * 3);
	TestTrue(TEXT("8.4 m wingspan"), FMath::IsNearlyEqual(Bounds.GetSize().Y, 840.0, 0.01));
	TestTrue(TEXT("8.8 m length"), FMath::IsNearlyEqual(Bounds.GetSize().X, 880.0, 0.01));
	TestTrue(TEXT("Muzzle at +650 cm remains clear"), Bounds.Max.X < 650.0);
	TestTrue(TEXT("Wing has modeled thickness, not a flat sheet"), WingMaxZ - WingMinZ > 10.0);
	TestTrue(TEXT("Upper wing normals point outwards"), bUpperWingFacesUp);
	TestTrue(TEXT("Left upper wing normals point outwards"), bLeftUpperWingFacesUp);
	TestTrue(TEXT("Right upper wing normals point outwards"), bRightUpperWingFacesUp);
	TestTrue(TEXT("Upper canopy normals point outwards"), bCanopyFacesUp);
	TestTrue(TEXT("Every upper canopy normal points outwards"), bUpperCanopyNormalsOutward);
	AddInfo(FString::Printf(TEXT("Original airframe: %d triangles, %d vertices, 6 mesh sections / 5 shared materials."),
		TriangleCount, VertexCount));
	return !HasAnyErrors();
}
#endif
