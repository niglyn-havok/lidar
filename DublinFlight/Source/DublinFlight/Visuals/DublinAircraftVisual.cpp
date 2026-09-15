#include "Visuals/DublinAircraftVisual.h"

namespace
{
	using namespace DublinAircraftVisual;

	struct FStation
	{
		double X;
		double Width;
		double Height;
		double Z;
	};

	struct FSurfaceVertex
	{
		FVector Position;
		FVector Normal;
		FVector Tangent;
		FVector2D UV;
		bool bFlipTangentY = false;
	};

	using FSurface = TFunctionRef<FVector(double, double)>;
	using FFinishSelector = TFunctionRef<EFinish(double, double)>;

	FSurfaceVertex Sample(FSurface Surface, double U, double V, bool bReverse)
	{
		constexpr double UStep = 0.001;
		constexpr double VStep = 0.00001;
		// Normalize derivatives, not tiny sample displacements: GetSafeNormal otherwise
		// treats valid lofts and thin trailing-edge sections as zero-length vectors.
		const FVector DU = (Surface(U + UStep, V) - Surface(U - UStep, V)) / (2.0 * UStep);
		const FVector DV = (Surface(U, V + VStep) - Surface(U, V - VStep)) / (2.0 * VStep);
		const FVector Tangent = DU.GetSafeNormal();
		const FVector VDirection = DV.GetSafeNormal();
		const FVector Normal = FVector::CrossProduct(VDirection, Tangent).GetSafeNormal() * (bReverse ? -1.0 : 1.0);
		return {Surface(U, V), Normal, Tangent, FVector2D(U, V),
			FVector::DotProduct(FVector::CrossProduct(Normal, Tangent), VDirection) < 0.0};
	}

	void AddTriangle(FSection& Section, int32 A, int32 B, int32 C)
	{
		// Unreal's visible clockwise face normal is (C-A) x (B-A).
		const FVector Face = FVector::CrossProduct(Section.Vertices[C] - Section.Vertices[A],
			Section.Vertices[B] - Section.Vertices[A]);
		if (FVector::DotProduct(Face, Section.Normals[A] + Section.Normals[B] + Section.Normals[C]) < 0.0)
		{
			Swap(B, C);
		}
		Section.Triangles.Append({A, B, C});
	}

	int32 AddVertex(FSection& Section, const FSurfaceVertex& Vertex)
	{
		const int32 Index = Section.Vertices.Add(Vertex.Position);
		Section.Normals.Add(Vertex.Normal);
		Section.UV.Add(Vertex.UV);
		Section.Tangents.Emplace(Vertex.Tangent, Vertex.bFlipTangentY);
		return Index;
	}

	void AddSurface(TArray<FSection>& Sections, const TArray<double>& Rows, int32 Slices,
		double VMax, FSurface Surface, FFinishSelector Finish, bool bReverse = false)
	{
		for (int32 Row = 0; Row + 1 < Rows.Num(); ++Row)
		{
			for (int32 Slice = 0; Slice < Slices; ++Slice)
			{
				const double U0 = Rows[Row], U1 = Rows[Row + 1];
				const double V0 = VMax * Slice / Slices, V1 = VMax * (Slice + 1) / Slices;
				FSection& Section = Sections[static_cast<int32>(Finish((U0 + U1) * 0.5, (V0 + V1) * 0.5))];
				const int32 A = AddVertex(Section, Sample(Surface, U0, V0, bReverse));
				const int32 B = AddVertex(Section, Sample(Surface, U1, V0, bReverse));
				const int32 C = AddVertex(Section, Sample(Surface, U1, V1, bReverse));
				const int32 D = AddVertex(Section, Sample(Surface, U0, V1, bReverse));
				AddTriangle(Section, A, B, C);
				AddTriangle(Section, A, C, D);
			}
		}
	}

	void AddCap(FSection& Section, FSurface Surface, double U, int32 Slices, const FVector& Normal)
	{
		FVector Center = FVector::ZeroVector;
		for (int32 Index = 0; Index < Slices; ++Index)
		{
			Center += Surface(U, static_cast<double>(Index) / Slices);
		}
		Center /= Slices;
		const FVector Tangent = FVector::CrossProduct(Normal,
			FMath::Abs(Normal.Z) < 0.9 ? FVector::UpVector : FVector::RightVector).GetSafeNormal();
		const FVector Bitangent = FVector::CrossProduct(Normal, Tangent);
		const int32 Middle = AddVertex(Section, {Center, Normal, Tangent, FVector2D::ZeroVector});
		for (int32 Index = 0; Index <= Slices; ++Index)
		{
			const double V = static_cast<double>(Index) / Slices;
			const FVector Position = Surface(U, V);
			AddVertex(Section, {Position, Normal, Tangent,
				FVector2D(FVector::DotProduct(Position - Center, Tangent),
					FVector::DotProduct(Position - Center, Bitangent)) / 100.0});
			if (Index > 0)
			{
				AddTriangle(Section, Middle, Middle + Index, Middle + Index + 1);
			}
		}
	}

	FVector LoftPoint(const TArray<FStation>& Stations, double X, double V)
	{
		int32 Index = 0;
		while (Index + 2 < Stations.Num() && X > Stations[Index + 1].X)
		{
			++Index;
		}
		const FStation& A = Stations[Index];
		const FStation& B = Stations[Index + 1];
		const double Alpha = FMath::Clamp((X - A.X) / (B.X - A.X), 0.0, 1.0);
		const double Angle = V * UE_TWO_PI;
		return FVector(FMath::Clamp(X, Stations[0].X, Stations.Last().X),
			FMath::Lerp(A.Width, B.Width, Alpha) * FMath::Cos(Angle),
			FMath::Lerp(A.Z, B.Z, Alpha) + FMath::Lerp(A.Height, B.Height, Alpha) * FMath::Sin(Angle));
	}

	void AddLoft(TArray<FSection>& Sections, const TArray<FStation>& Stations, int32 Slices,
		FFinishSelector Finish, bool bCanopy = false)
	{
		TArray<double> Rows;
		for (const FStation& Station : Stations)
		{
			Rows.Add(Station.X);
		}
		const auto Surface = [&Stations](double X, double V) { return LoftPoint(Stations, X, V); };
		AddSurface(Sections, Rows, Slices, bCanopy ? 0.5 : 1.0, Surface, Finish);
		if (!bCanopy)
		{
			AddCap(Sections[static_cast<int32>(Finish(Rows[0], 0.0))], Surface, Rows[0], Slices, -FVector::ForwardVector);
			AddCap(Sections[static_cast<int32>(Finish(Rows.Last(), 0.0))], Surface, Rows.Last(), Slices, FVector::ForwardVector);
		}
	}

	// An original closed teardrop section, not a flight/engineering airfoil model.
	FVector2D Airfoil(double V, double Thickness, double Camber)
	{
		const double Angle = V * UE_TWO_PI;
		const double Chord = (1.0 - FMath::Cos(Angle)) * 0.5;
		return FVector2D(Chord, Camber * FMath::Sin(UE_PI * Chord)
			+ Thickness * FMath::Sin(Angle) * (0.68 + 0.32 * FMath::Cos(Angle)));
	}

	void AddWings(TArray<FSection>& Sections, bool bTail)
	{
		const TArray<double> Rows = bTail
			? TArray<double>{0.0, 0.2, 0.4, 0.6, 0.8, 0.94, 1.0}
			: TArray<double>{0.0, 0.12, 0.25, 0.4, 0.55, 0.7, 0.73, 0.76, 0.85, 0.94, 1.0};
		const int32 Slices = bTail ? 24 : 32;
		const auto Finish = [bTail](double U, double V)
		{
			if (!bTail && U >= 0.7 && U < 0.73) { return EFinish::Brass; }
			if (U >= 0.76 || bTail) { return EFinish::Petrol; }
			if (V > 0.45 && V < 0.55) { return EFinish::Graphite; }
			return EFinish::Pearl;
		};
		for (double Side : {-1.0, 1.0})
		{
			const auto Surface = [bTail, Side](double U, double V)
			{
				const double Span = bTail ? 154.0 : 420.0;
				const double Chord = (bTail ? 126.0 : 225.0)
					* (1.0 - 0.4 * U - 0.5 * FMath::Pow(U, 8.0));
				const double Leading = (bTail ? -276.0 : 105.0) - (bTail ? 28.0 : 75.0) * U;
				const FVector2D Profile = Airfoil(V, bTail ? 0.045 : 0.065, bTail ? 0.01 : 0.025);
				return FVector(Leading - Chord * Profile.X, Side * Span * U,
					(bTail ? 20.0 : -18.0) + (bTail ? 10.0 : 20.0) * U + Chord * Profile.Y);
			};
			AddSurface(Sections, Rows, Slices, 1.0, Surface, Finish, Side > 0.0);
			AddCap(Sections[static_cast<int32>(EFinish::Petrol)], Surface, 1.0, Slices, FVector(0.0, Side, 0.0));
			AddCap(Sections[static_cast<int32>(bTail ? EFinish::Petrol : EFinish::Pearl)],
				Surface, 0.0, Slices, FVector(0.0, -Side, 0.0));
		}
	}
}

DublinAircraftVisual::FGeometry DublinAircraftVisual::BuildGeometry()
{
	FGeometry Geometry;
	Geometry.Airframe.SetNum(FinishCount);
	for (int32 Index = 0; Index < FinishCount; ++Index)
	{
		Geometry.Airframe[Index].Finish = static_cast<EFinish>(Index);
	}

	AddLoft(Geometry.Airframe, {
		{-435, 2, 3, 8}, {-418, 10, 12, 8}, {-370, 17, 21, 7}, {-310, 24, 29, 6},
		{-235, 33, 38, 3}, {-160, 42, 46, 0}, {-70, 49, 52, 0}, {35, 55, 57, 0},
		{135, 56, 57, -1}, {220, 51, 50, -2}, {282, 43, 42, -1}, {305, 41, 40, 0},
		{323, 39, 37, 0}, {347, 34, 32, 0}, {373, 28, 27, 0}, {385, 25, 24, 0}
	}, 32, [](double X, double V)
	{
		if (X > 305 && X < 323) { return EFinish::Graphite; }
		if (X > 282) { return EFinish::Petrol; }
		if (X > -370 && (V < 0.03125 || (V > 0.46875 && V < 0.5))) { return EFinish::Brass; }
		return V > 0.5625 && V < 0.9375 ? EFinish::Petrol : EFinish::Pearl;
	});
	AddLoft(Geometry.Airframe, {
		{385, 25, 24, 0}, {400, 23, 22, 0}, {421, 17, 16, 0}, {438, 9, 8, 0}, {445, 1, 1, 0}
	}, 32, [](double X, double V) { return X > 438 ? EFinish::Brass : EFinish::Graphite; });

	AddWings(Geometry.Airframe, false);
	AddWings(Geometry.Airframe, true);

	const auto Fin = [](double U, double V)
	{
		const double Chord = 140.0 - 90.0 * U;
		const FVector2D Profile = Airfoil(V, 0.048, 0.0);
		return FVector(-268.0 - 53.0 * U - Chord * Profile.X, Chord * Profile.Y, 25.0 + 148.0 * U);
	};
	AddSurface(Geometry.Airframe, {0.0, 0.16, 0.32, 0.48, 0.64, 0.8, 0.92, 1.0}, 24, 1.0,
		Fin, [](double U, double V) { return U > 0.8 && U < 0.92 ? EFinish::Brass : EFinish::Petrol; });
	AddCap(Geometry.Airframe[static_cast<int32>(EFinish::Petrol)], Fin, 1.0, 24, FVector::UpVector);
	AddCap(Geometry.Airframe[static_cast<int32>(EFinish::Petrol)], Fin, 0.0, 24, -FVector::UpVector);

	// Frames are material regions of the shell: no coplanar decals or layered glass.
	AddLoft(Geometry.Airframe, {
		{-130, 12, 6, 39}, {-110, 28, 20, 43}, {-78, 39, 37, 45}, {-73, 39.5, 38.5, 45.2},
		{-15, 42, 55, 47}, {50, 42, 58, 49}, {108, 35.5, 44.5, 48.1},
		{114, 34.5, 42, 47.9}, {151, 26.5, 26, 46.2}, {157, 24, 22, 45.8}, {184, 6, 3, 44}
	}, 24, [](double X, double V)
	{
		if ((X > -78 && X < -73) || (X > 108 && X < 114) || (X > 151 && X < 157)
			|| V < 0.021 || V > 0.479 || (V > 0.229 && V < 0.271))
		{
			return EFinish::Graphite;
		}
		return EFinish::Canopy;
	}, true);

	TArray<FSection> RotorSections;
	RotorSections.SetNum(FinishCount);
	for (int32 Blade = 0; Blade < 3; ++Blade)
	{
		const double Angle = Blade * UE_TWO_PI / 3.0;
		const auto Rotor = [Angle](double U, double V)
		{
			const double Chord = 25.0 - 13.0 * U;
			const FVector2D Profile = Airfoil(V, 0.10, 0.0);
			const double Radius = 18.0 + 108.0 * U;
			const double Sweep = 5.0 + 11.0 * U - Chord * Profile.X;
			return FVector(Chord * Profile.Y, Radius * FMath::Cos(Angle) - Sweep * FMath::Sin(Angle),
				Radius * FMath::Sin(Angle) + Sweep * FMath::Cos(Angle));
		};
		AddSurface(RotorSections, {0.0, 0.2, 0.4, 0.6, 0.8, 1.0}, 16, 1.0,
			Rotor, [](double U, double V) { return EFinish::Graphite; });
		const FVector TipNormal(0.0, FMath::Cos(Angle), FMath::Sin(Angle));
		AddCap(RotorSections[static_cast<int32>(EFinish::Graphite)], Rotor, 1.0, 16, TipNormal);
		AddCap(RotorSections[static_cast<int32>(EFinish::Graphite)], Rotor, 0.0, 16, -TipNormal);
	}
	Geometry.Propeller = MoveTemp(RotorSections[static_cast<int32>(EFinish::Graphite)]);
	Geometry.Propeller.Finish = EFinish::Graphite;
	return Geometry;
}

DublinAircraftVisual::FFinish DublinAircraftVisual::GetFinish(EFinish Finish)
{
	switch (Finish)
	{
	case EFinish::Pearl: return {FLinearColor(0.68f, 0.72f, 0.69f), 0.34f};
	case EFinish::Petrol: return {FLinearColor(0.018f, 0.24f, 0.29f), 0.32f};
	case EFinish::Graphite: return {FLinearColor(0.018f, 0.027f, 0.033f), 0.38f};
	case EFinish::Canopy: return {FLinearColor(0.008f, 0.026f, 0.045f), 0.12f};
	case EFinish::Brass: return {FLinearColor(0.72f, 0.43f, 0.12f), 0.36f};
	default: checkNoEntry(); return {FLinearColor::Black, 1.0f};
	}
}
