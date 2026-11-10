// Experimental static Mii head conversion. See docs/STATUS.md for its scope.
using System;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Web.Script.Serialization;
using BfresLibrary;
using BfresLibrary.Helpers;
using BfresLibrary.Switch;
using BfresLibrary.GX2;
using Syroot.Maths;
using Syroot.NintenTools.NSW.Bntx;
using Syroot.NintenTools.NSW.Bntx.GFX;

public class HeadPart { public string name,texture; public float[][] positions,normals,uv; public uint[] indices; public float[] color; public bool transparent; }
public class HeadTexture { public string name; public uint width,height; }
public class HeadData { public HeadPart[] parts; public HeadTexture[] textures; }
public class MiiHeadBuilder {
    static bool Finite(float n) { return !Single.IsNaN(n) && !Single.IsInfinity(n); }
    static void Validate(HeadData data,string root) {
        if(data==null || data.parts==null || data.parts.Length<3 || data.parts.Length>4 || data.textures==null || data.textures.Length>8)
            throw new InvalidDataException("Expected face, nose, hair, optional forehead and at most eight textures");
        var names=new HashSet<string>();
        foreach(var t in data.textures) {
            if(t==null || t.name==null || !Regex.IsMatch(t.name,@"\A[a-z0-9_]{1,32}\z") || !names.Add(t.name)
                || t.width<2 || t.height<2 || t.width>256 || t.height>256)
                throw new InvalidDataException("Invalid texture name or dimensions");
            if(new FileInfo(Path.Combine(root,t.name+".rgba")).Length!=4L*t.width*t.height)
                throw new InvalidDataException("RGBA size mismatch");
        }
        if(!names.Contains("flatnormal") || !names.Contains("black") || !names.Contains("white"))
            throw new InvalidDataException("Missing neutral material maps");
        var expected=new HashSet<string>{"Faceline","Nose","Forehead","Hair"};
        foreach(var p in data.parts) {
            if(p==null || !expected.Remove(p.name) || p.transparent || !names.Contains(p.texture)
                || p.positions==null || p.positions.Length<3 || p.positions.Length>16000
                || p.normals==null || p.normals.Length!=p.positions.Length
                || p.uv==null || p.uv.Length!=p.positions.Length || p.indices==null
                || p.indices.Length<3 || p.indices.Length>96000 || p.indices.Length%3!=0
                || p.color==null || p.color.Length!=4 || p.color.Any(v=>!Finite(v)||v<0||v>1))
                throw new InvalidDataException("Invalid baked head part");
            for(int i=0;i<p.positions.Length;i++) {
                if(p.positions[i]==null || p.positions[i].Length!=3 || p.positions[i].Any(v=>!Finite(v)||Math.Abs(v)>32)
                    || p.normals[i]==null || p.normals[i].Length!=3 || p.normals[i].Any(v=>!Finite(v)||Math.Abs(v)>1.01f)
                    || p.uv[i]==null || p.uv[i].Length!=2 || p.uv[i].Any(v=>!Finite(v)||v<0||v>1))
                    throw new InvalidDataException("Invalid vertex");
                double length=p.normals[i].Sum(v=>(double)v*v);
                if(length<0.9 || length>1.1) throw new InvalidDataException("Expected unit normals");
            }
            if(p.indices.Any(i=>i>=p.positions.Length)) throw new InvalidDataException("Index outside vertex buffer");
        }
        if(expected.Any(name=>name!="Forehead")) throw new InvalidDataException("Missing required opaque head part");
    }
    public static void Main(string[] args) {
        if(args.Length!=3) throw new ArgumentException("Usage: MiiHeadBuilder original-MiiM.bfres baked-head.json output.bfres");
        if(File.Exists(args[2])) throw new IOException("Output exists; choose a new derived file");
        if(new DriveInfo(Path.GetPathRoot(Path.GetFullPath(args[2]))).AvailableFreeSpace < 4L*1024*1024*1024)
            throw new IOException("At least 4 GiB free required");
        using(var sha=SHA256.Create()) using(var input=File.OpenRead(args[0])) {
            var hash=BitConverter.ToString(sha.ComputeHash(input)).Replace("-", "").ToLowerInvariant();
            if(hash!="d2d4f0992ab12c0773e0a89add358576c2ceccff0450deb5053740f73c57568a")
                throw new InvalidDataException("Unsupported original MiiM BFRES; expected verified MK8 4.0.1 asset");
        }
        if(new FileInfo(args[1]).Length>8*1024*1024) throw new InvalidDataException("Head JSON too large");
        string root=Path.GetDirectoryName(Path.GetFullPath(args[1]));
        var data=new JavaScriptSerializer { MaxJsonLength=8*1024*1024 }.Deserialize<HeadData>(File.ReadAllText(args[1]));
        Validate(data,root);
        var res=new ResFile(args[0]); var body=res.Models["Mii"];
        int head=body.Skeleton.Bones.Keys.ToList().IndexOf("Head");
        if(head!=10 || body.Shapes.Count!=1 || body.Materials.Count!=1) throw new Exception("No Head bone");
        var bntx=new BntxFile(res.ExternalFiles["textures.bntx"].GetStream(false));
        foreach(var image in data.textures) {
            var t=new Syroot.NintenTools.NSW.Bntx.Texture {
                Name="MK8_"+image.name, Width=image.width,Height=image.height,Depth=1,ArrayLength=1,MipCount=1,
                Format=image.name=="flatnormal"?SurfaceFormat.R8_G8_B8_A8_SNORM:SurfaceFormat.R8_G8_B8_A8_SRGB,Dim=Dim.Dim2D,SurfaceDim=SurfaceDim.Dim2D,
                TileMode=TileMode.Default,Alignment=512,BlockHeightLog2=0,textureLayout=0,textureLayout2=65543,
                Flags=1, SampleCount=1,AccessFlags=AccessFlags.Texture,
                ChannelRed=ChannelType.Red,ChannelGreen=ChannelType.Green,ChannelBlue=ChannelType.Blue,ChannelAlpha=ChannelType.Alpha,
                MipOffsets=new long[1], TextureData=new List<List<byte[]>> {new List<byte[]> {new byte[0]}}
            };
            var wrapper=new SwitchTexture(bntx,t); wrapper.Texture=t; wrapper.BntxFile=bntx;
            wrapper.SwizzleImage(File.ReadAllBytes(Path.Combine(root,image.name+".rgba")),0);
            bntx.Textures.Add(t); bntx.TextureDict.Add(t.Name);
        }
        foreach(var part in data.parts) {
            var template=new ResFile(args[0]).Models["Helmet"];
            string type="m_Helmet";
            var mat=template.Materials[type]; mat.Name="MK8_"+part.name;
            mat.TextureRefs[0].Name="MK8_"+part.texture;
            mat.TextureRefs[1].Name="MK8_flatnormal";
            mat.TextureRefs[2].Name="MK8_black";
            mat.TextureRefs[3].Name="MK8_white";
            mat.ShaderParams["specular_intensity"].DataValue=0.01f;
            mat.ShaderParams["transparency"].DataValue=1.0f;
            mat.ShaderParams["albedo_tex_color"].DataValue=part.color.Take(3).ToArray();
            var shape=template.Shapes.Values.First(x=>x.MaterialIndex==template.Materials.IndexOf(type));
            var vh=new VertexBufferHelper(template.VertexBuffers[shape.VertexBufferIndex],res.ByteOrder);
            int n=part.positions.Length;
            foreach(var a in vh.Attributes) {
                a.Data=new Vector4F[n];
                if(a.Name=="_u0") a.Format=GX2AttribFormat.Format_32_32_Single;
                for(int i=0;i<n;i++) {
                    if(a.Name=="_p0") a.Data[i]=new Vector4F(part.positions[i][0],part.positions[i][1],part.positions[i][2],1);
                    else if(a.Name=="_n0") a.Data[i]=new Vector4F(part.normals[i][0],part.normals[i][1],part.normals[i][2],0);
                    else if(a.Name=="_u0") a.Data[i]=new Vector4F(part.uv[i][0],part.uv[i][1],0,0);
                    else if(a.Name=="_t0" || a.Name=="_b0") {
                        var normal=new Vector3F(part.normals[i][0],part.normals[i][1],part.normals[i][2]);
                        var axis=Math.Abs(normal.X)<0.9f?new Vector3F(1,0,0):new Vector3F(0,1,0);
                        var tangent=axis.Cross(normal); tangent=tangent/(float)Math.Sqrt(tangent.X*tangent.X+tangent.Y*tangent.Y+tangent.Z*tangent.Z);
                        var v=a.Name=="_t0"?tangent:normal.Cross(tangent);
                        a.Data[i]=new Vector4F(v.X,v.Y,v.Z,1);
                    }
                    else if(a.Name=="_c0") a.Data[i]=new Vector4F(1,1,1,1);
                }
            }
            shape.Name="MK8_"+part.name; shape.BoneIndex=(ushort)head; shape.VertexSkinCount=0;
            shape.MaterialIndex=(ushort)body.Materials.Count; shape.VertexBufferIndex=(ushort)body.VertexBuffers.Count;
            shape.SkinBoneIndices.Clear(); vh.VertexSkinCount=0;
            var mesh=shape.Meshes[0]; mesh.FirstVertex=0; mesh.SetIndices(part.indices,GX2IndexFormat.UInt16LittleEndian);
            mesh.SubMeshes=new List<SubMesh>{new SubMesh {Offset=0,Count=(uint)part.indices.Length}};
            shape.Meshes=new List<Mesh>{mesh};
            var min=new Vector3F(part.positions.Min(v=>v[0]),part.positions.Min(v=>v[1]),part.positions.Min(v=>v[2]));
            var max=new Vector3F(part.positions.Max(v=>v[0]),part.positions.Max(v=>v[1]),part.positions.Max(v=>v[2]));
            shape.SubMeshBoundings=new List<Bounding>{new Bounding {Center=(min+max)*0.5f,Extent=(max-min)*0.5f}};
            shape.RadiusArray=new List<float>{20};
            body.Materials.Add(mat.Name,mat);body.VertexBuffers.Add(vh.ToVertexBuffer());body.Shapes.Add(shape.Name,shape);
        }
        using(var stream=new MemoryStream()) { bntx.Save(stream); res.ExternalFiles["textures.bntx"].Data=stream.ToArray(); }
        res.Save(args[2]);
        var check=new ResFile(args[2]);
        Console.WriteLine("Saved head on bone {0}, shapes {1}, materials {2}",head,check.Models["Mii"].Shapes.Count,check.Models["Mii"].Materials.Count);
    }
}






